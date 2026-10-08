import assert from 'node:assert/strict'
import { mkdir, readFile, readdir, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { setTimeout as delay } from 'node:timers/promises'

export async function createFolderTask({ page, click, until }) {
    await click('.primary-button')
    await until(
        () => page.evaluate("!!document.querySelector('.choose-folders')"),
        'source composer',
    )
    await click('.choose-folders')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.create-task')?.disabled === false",
            ),
        'folder scope checked',
    )
    await click('.create-task')
    await until(
        () => page.evaluate("!document.querySelector('dialog[open]')"),
        'folder task saved',
    )
}

export async function checkSources({ page, main, click, until, temporary }) {
    const source = join(temporary, 'chosen-sources')
    const longName = 'selected-' + 'long-name-'.repeat(15) + '.txt'
    const singlePath = join(source, longName)
    await mkdir(join(source, 'left', 'empty'), { recursive: true })
    await mkdir(join(source, 'right'), { recursive: true })
    await writeFile(singlePath, 'single file content')
    await writeFile(join(source, 'left', 'same.txt'), 'left content')
    await writeFile(join(source, 'right', 'same.txt'), 'right content')
    await writeFile(join(source, 'left', 'ignore.txt'), 'not selected')
    await writeFile(join(source, 'right', 'ignore.txt'), 'not selected')
    await click('[data-tab="tasks"]')

    async function open() {
        await click('.primary-button')
        await until(
            () => page.evaluate("!!document.querySelector('.choose-files')"),
            'new task dialog',
        )
    }
    async function choose(kind, paths) {
        await main.evaluate(`testElectron.dialog.showOpenDialog = async (_window, options) => {
            globalThis.lastSourceDialogOptions = options;
            return { canceled: ${paths === null}, filePaths: ${JSON.stringify(paths || [])} };
        }`)
        await click(`.choose-${kind}`)
        await until(
            () =>
                page.evaluate(
                    "!document.querySelector('.choose-files').disabled",
                ),
            'source chooser returned',
        )
    }
    async function save() {
        await until(
            () =>
                page.evaluate(
                    "document.querySelector('.create-task')?.disabled === false",
                ),
            'selected scope valid',
        )
        await click('.create-task')
        await until(
            () => page.evaluate("!document.querySelector('dialog[open]')"),
            'selected task created',
        )
    }
    async function capture(name, selector) {
        for (const [width, height] of [
            [1120, 760],
            [850, 600],
        ]) {
            await main.evaluate(`testWindow.setSize(${width}, ${height})`)
            await delay(150)
            if (name === 'task') {
                await page.evaluate(
                    "document.querySelector('.task-card:last-child').scrollIntoView({ block: 'start' })",
                )
            }
            assert(
                await page.evaluate(
                    `document.documentElement.scrollWidth <= innerWidth && [...document.querySelectorAll(${JSON.stringify(selector)})].every(element => element.scrollWidth <= element.clientWidth + 1)`,
                ),
                'selection layout fits',
            )
            const capture = await page.call('Page.captureScreenshot', {
                format: 'png',
            })
            await writeFile(
                join(tmpdir(), `backup-sources-${name}-${width}.png`),
                Buffer.from(capture.data, 'base64'),
            )
        }
    }
    async function roundtrip(task, destination) {
        const card = `.task-card[data-task-id="${task.id}"]`
        await click(`${card} .scan-button`)
        await until(
            () =>
                page.evaluate(
                    `document.querySelector(${JSON.stringify(card + ' .scan-backup')})?.disabled === false`,
                ),
            'selected scan complete',
        )
        await click(`${card} .scan-backup`)
        await until(
            () =>
                page.evaluate(
                    `document.querySelector(${JSON.stringify(card + ' .operation-panel h3')})?.textContent === '备份 · 成功'`,
                ),
            'selected backup complete',
        )
        await click(`${card} .view-versions`)
        await until(
            () =>
                page.evaluate(
                    "document.querySelectorAll('.versions-table tbody tr').length === 1",
                ),
            'selected version',
        )
        assert(
            (
                await page.evaluate(
                    "document.querySelector('.versions-table').textContent",
                )
            ).includes(`范围：选定 ${task.selection.length} 项`),
        )
        await click('.versions-table .source-details summary')
        await capture('version', '.versions-table')
        await click('.versions-table tbody button')
        await until(
            () => page.evaluate("!!document.querySelector('.restore-summary')"),
            'selected restore confirmation',
        )
        await click('.restore-summary .source-details summary')
        await capture('restore-dialog', '.workflow-dialog')
        await main.evaluate(
            `testElectron.dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [${JSON.stringify(destination)}] })`,
        )
        await click('[aria-label="选择还原目录"]')
        await until(
            () =>
                page.evaluate(
                    `document.querySelector('[aria-label="还原目录"]').value === ${JSON.stringify(destination)} && !document.querySelector('.confirm-restore').disabled`,
                ),
            'selected restore path ready',
        )
        await click('.confirm-restore')
        await until(
            () =>
                page.evaluate(
                    "document.querySelector('.operation-panel h2')?.textContent === '还原 · 成功'",
                ),
            'selected restore complete',
        )
    }

    await open()
    assert(
        await page.evaluate("document.querySelector('.create-task').disabled"),
    )
    await choose('files', [singlePath])
    assert.deepEqual(
        (await main.evaluate('lastSourceDialogOptions')).properties,
        ['openFile', 'multiSelections'],
    )
    await choose('files', null)
    assert.equal(
        await page.evaluate(
            "document.querySelectorAll('.source-list tbody tr').length",
        ),
        1,
    )
    await capture('single-composer', '.source-list, .workflow-dialog')
    await save()
    let config = await page.evaluate('window.backup.getConfig()')
    const single = config.tasks.find(
        (task) => task.selection?.[0]?.path === longName,
    )
    assert(
        single && !single.name,
        'long file name does not violate optional name limit',
    )
    assert.equal(
        await page.evaluate(
            `document.querySelector('[data-task-id="${single.id}"] h2').textContent`,
        ),
        longName,
    )
    const singleOutput = join(temporary, 'single-selected-output')
    await roundtrip(single, singleOutput)
    assert.deepEqual(await readdir(singleOutput), [longName])
    assert.equal(
        await readFile(join(singleOutput, longName), 'utf8'),
        'single file content',
    )
    console.log(
        'PASS: single-file composer, native multi-select options, cancellation, long names, backup and restore',
    )

    await click('[data-tab="tasks"]')
    await open()
    await choose('files', [
        join(source, 'left/same.txt'),
        join(source, 'right/same.txt'),
    ])
    await click('[aria-label="任务名称"]')
    await page.call('Input.insertText', { text: 'Selected documents' })
    const paths = await page.evaluate(
        "[...document.querySelectorAll('.source-list tbody tr td:nth-child(2)')].map(cell => cell.textContent)",
    )
    assert.deepEqual(paths, ['left/same.txt', 'right/same.txt'])
    await capture('multiple-composer', '.source-list, .workflow-dialog')
    await save()
    config = await page.evaluate('window.backup.getConfig()')
    const multiple = config.tasks.find(
        (task) => task.name === 'Selected documents',
    )
    assert.equal(multiple.selection.length, 2)
    const multiOutput = join(temporary, 'multiple-selected-output')
    await roundtrip(multiple, multiOutput)
    assert.deepEqual(await readdir(join(multiOutput, 'left')), ['same.txt'])
    assert.deepEqual(await readdir(join(multiOutput, 'right')), ['same.txt'])
    assert.equal(
        await readFile(join(multiOutput, 'left/same.txt'), 'utf8'),
        'left content',
    )
    assert.equal(
        await readFile(join(multiOutput, 'right/same.txt'), 'utf8'),
        'right content',
    )
    console.log(
        'PASS: named multi-file task preserves same-name restore paths and excludes neighboring files',
    )

    await click('[data-tab="tasks"]')
    await open()
    await choose('files', [singlePath, join(source, 'left/same.txt')])
    await choose('folders', [join(source, 'left')])
    assert.deepEqual(
        (await main.evaluate('lastSourceDialogOptions')).properties,
        ['openDirectory', 'multiSelections'],
    )
    assert.equal(
        await page.evaluate(
            "document.querySelectorAll('.source-list tbody tr').length",
        ),
        2,
    )
    assert(
        (
            await page.evaluate("document.querySelector('dialog').textContent")
        ).includes('已合并 1 项'),
    )
    await click(
        `.source-list tr[data-source-path="${singlePath}"] .remove-source`,
    )
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.source-list tbody tr td:nth-child(2)').textContent === '目录内原有结构'",
            ),
        'single folder restore layout',
    )
    await choose('files', [singlePath])
    await choose('files', [join(source, 'missing')])
    await until(
        () => page.evaluate("!!document.querySelector('dialog [role=alert]')"),
        'invalid selection feedback',
    )
    assert(
        await page.evaluate("document.querySelector('.create-task').disabled"),
    )
    await click(
        `.source-list tr[data-source-path="${join(source, 'missing')}"] .remove-source`,
    )
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.create-task')?.disabled === false",
            ),
        'remove invalid source recovers',
    )
    await capture('mixed-composer', '.source-list, .workflow-dialog')
    await save()
    config = await page.evaluate('window.backup.getConfig()')
    const mixed = config.tasks.find((task) =>
        task.selection?.some((item) => item.type === 'directory'),
    )
    assert(mixed && mixed.selection.length === 2)
    await click(`[data-task-id="${mixed.id}"] .source-details summary`)
    await capture('task', '.task-card')
    await page.call('Page.reload')
    await until(
        () =>
            page.evaluate(
                `!!document.querySelector('[data-task-id="${mixed.id}"]')`,
            ),
        'selected tasks after reload',
    )
    assert.deepEqual(
        (await page.evaluate('window.backup.getConfig()')).tasks,
        config.tasks,
    )
    console.log(
        'PASS: mixed file/folder sources merge covered children, support removal and error recovery, persist across reload, and fit both window sizes',
    )
}
