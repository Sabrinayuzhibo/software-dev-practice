import assert from 'node:assert/strict'
import { execFile } from 'node:child_process'
import {
    lstat,
    mkdir,
    readFile,
    readdir,
    symlink,
    writeFile,
} from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { setTimeout as delay } from 'node:timers/promises'
import { fileURLToPath } from 'node:url'
import { promisify } from 'node:util'

export async function checkTypeFilter({ page, main, click, until, temporary }) {
    const source = join(temporary, 'type-filter-source')
    const regular = join(source, 'nested', 'keep.txt')
    const linked = join(source, 'selected-link')
    const destination = join(temporary, 'type-filter-output')
    await mkdir(join(source, 'nested'), { recursive: true })
    await mkdir(join(source, 'empty'))
    await writeFile(regular, 'selected content\n')
    await symlink('nested/keep.txt', linked)
    await promisify(execFile)('mkfifo', [join(source, 'pipe')])

    async function open() {
        await click('[data-tab="tasks"]')
        await click('.primary-button')
        await until(
            () =>
                page.evaluate("!!document.querySelector('.file-type-picker')"),
            'file type selector',
        )
    }

    async function choose(kind, paths) {
        await main.evaluate(
            `testElectron.dialog.showOpenDialog = async () => ({ canceled: false, filePaths: ${JSON.stringify(paths)} })`,
        )
        await click(`.choose-${kind}`)
        await until(
            () =>
                page.evaluate(
                    "!document.querySelector('.choose-files').disabled",
                ),
            'type-filtered source check',
        )
    }

    async function capture(name) {
        for (const [width, height] of [
            [1120, 760],
            [850, 600],
        ]) {
            await main.evaluate(`testWindow.setSize(${width}, ${height})`)
            await delay(150)
            assert(
                await page.evaluate(
                    `(() => {
                        const dialog = document.querySelector('.workflow-dialog');
                        const footer = dialog.querySelector('.dialog-actions').getBoundingClientRect();
                        const error = dialog.querySelector('[role=alert]')?.getBoundingClientRect();
                        return document.documentElement.scrollWidth <= innerWidth &&
                            dialog.scrollWidth <= dialog.clientWidth &&
                            footer.bottom <= dialog.getBoundingClientRect().bottom &&
                            (!error || error.bottom <= footer.top);
                    })()`,
                ),
                'file type dialog and error fit above visible actions',
            )
            const screenshot = await page.call('Page.captureScreenshot', {
                format: 'png',
            })
            await writeFile(
                join(tmpdir(), `backup-type-filter-${name}-${width}.png`),
                Buffer.from(screenshot.data, 'base64'),
            )
        }
    }

    await open()
    assert(
        await page.evaluate(
            "document.querySelector('.empty-directory-option input').checked",
        ),
        'empty directories are preserved by default',
    )
    for (const type of ['symlink', 'fifo']) {
        await click(`[data-file-type="${type}"] input`)
    }
    await click('.empty-directory-option input')
    await choose('folders', [source])
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.create-task')?.disabled === false",
            ),
        'filtered folder ready',
    )
    await capture('folder-composer')
    await click('.create-task')
    await until(
        () => page.evaluate("!document.querySelector('dialog[open]')"),
        'filtered folder task saved',
    )
    const tasks = (await page.evaluate('window.backup.getConfig()')).tasks
    const folderTask = tasks.find((task) => task.path === source)
    assert.deepEqual(folderTask?.file_types, ['file'])
    assert.equal(folderTask?.preserve_empty_dirs, false)
    const card = `.task-card[data-task-id="${folderTask.id}"]`
    assert(
        (
            await page.evaluate(
                `document.querySelector(${JSON.stringify(card)}).textContent`,
            )
        ).includes('类型：普通文件 · 不保留空目录'),
    )
    await click(`${card} .scan-button`)
    await until(
        () =>
            page.evaluate(
                `document.querySelector(${JSON.stringify(card + ' .scan-backup')})?.disabled === false`,
            ),
        'filtered folder scan',
    )
    const preview = await page.evaluate(
        `[...document.querySelectorAll(${JSON.stringify(card + ' .scan-table tbody tr')})].map(row => [row.dataset.type, row.querySelector('td').textContent])`,
    )
    assert.deepEqual(preview, [
        ['directory', 'nested'],
        ['file', 'nested/keep.txt'],
    ])
    await click(`${card} .scan-backup`)
    await until(
        () =>
            page.evaluate(
                `document.querySelector(${JSON.stringify(card + ' .operation-panel h3')})?.textContent === '备份 · 成功'`,
            ),
        'filtered folder backup',
    )
    await click(`${card} .view-versions`)
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.versions-table tbody tr').length === 1",
            ),
        'filtered version',
    )
    assert(
        (
            await page.evaluate(
                "document.querySelector('.versions-table').textContent",
            )
        ).includes('类型：普通文件 · 不保留空目录'),
    )
    await click('.versions-table tbody button')
    await until(
        () => page.evaluate("!!document.querySelector('.restore-summary')"),
        'filtered restore confirmation',
    )
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [${JSON.stringify(destination)}] })`,
    )
    await click('[aria-label="选择还原目录"]')
    await until(
        () =>
            page.evaluate(
                "!document.querySelector('.confirm-restore').disabled",
            ),
        'filtered restore destination',
    )
    await click('.confirm-restore')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.operation-panel h2')?.textContent === '还原 · 成功'",
            ),
        'filtered restore complete',
    )
    assert.deepEqual(await readdir(destination), ['nested'])
    assert.deepEqual(await readdir(join(destination, 'nested')), ['keep.txt'])
    assert.equal(
        await readFile(join(destination, 'nested/keep.txt'), 'utf8'),
        'selected content\n',
    )
    console.log(
        'PASS: folder type picker filters scan, backup and restore; rules persist in task and version',
    )

    await open()
    await click('.all-file-types input')
    assert(
        await page.evaluate("document.querySelector('.create-task').disabled"),
    )
    await click('[data-file-type="symlink"] input')
    await choose('files', [regular, linked])
    await until(
        () => page.evaluate("!!document.querySelector('dialog [role=alert]')"),
        'direct selection type mismatch',
    )
    assert(
        (
            await page.evaluate(
                "document.querySelector('dialog [role=alert]').textContent",
            )
        ).includes('所选文件的类型不在当前筛选范围内'),
    )
    assert(
        await page.evaluate("document.querySelector('.create-task').disabled"),
    )
    await capture('direct-mismatch')
    await click(`.source-list tr[data-source-path="${regular}"] .remove-source`)
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.create-task')?.disabled === false",
            ),
        'remove mismatching file',
    )
    await click('[data-file-type="file"] input')
    await until(
        () =>
            page.evaluate(
                "!document.querySelector('.file-type-picker').disabled",
            ),
        'combined file and link filter checked',
    )
    await click('[data-file-type="symlink"] input')
    await until(
        () => page.evaluate("!!document.querySelector('dialog [role=alert]')"),
        'changing filter invalidates selected link',
    )
    assert(
        await page.evaluate("document.querySelector('.create-task').disabled"),
    )
    await click('[data-file-type="symlink"] input')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.create-task')?.disabled === false",
            ),
        'changing filter restores selected link',
    )
    await click('[data-file-type="file"] input')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.create-task')?.disabled === false",
            ),
        'symlink-only selection ready',
    )
    await click('.create-task')
    await until(
        () => page.evaluate("!document.querySelector('dialog[open]')"),
        'matching link task saved',
    )
    const linkTask = (
        await page.evaluate('window.backup.getConfig()')
    ).tasks.find((task) =>
        task.selection?.some((item) => item.path === 'selected-link'),
    )
    assert.deepEqual(linkTask?.file_types, ['symlink'])
    assert.equal(linkTask?.preserve_empty_dirs, true)
    assert((await lstat(linked)).isSymbolicLink())
    console.log(
        'PASS: every directly selected file is checked before task creation; removing a mismatch recovers',
    )

    const future = join(temporary, 'type-filter-future')
    await mkdir(join(future, 'nested'), { recursive: true })
    await writeFile(join(future, 'ignored.txt'), 'not a link')
    await open()
    await click('.all-file-types input')
    await click('[data-file-type="symlink"] input')
    await choose('folders', [future])
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.create-task')?.disabled === false",
            ),
        'future matching files task allowed',
    )
    await click('.create-task')
    await until(
        () => page.evaluate("!document.querySelector('dialog[open]')"),
        'future task saved',
    )
    const futureTask = (
        await page.evaluate('window.backup.getConfig()')
    ).tasks.find((task) => task.path === future)
    assert.equal(futureTask?.preserve_empty_dirs, true)
    const futureCard = `.task-card[data-task-id="${futureTask.id}"]`
    await click(`${futureCard} .scan-button`)
    await until(
        () =>
            page.evaluate(
                `document.querySelector(${JSON.stringify(futureCard + ' .scan-empty')})?.textContent === '本次没有匹配文件'`,
            ),
        'no matching files message',
    )
    assert(
        await page.evaluate(
            `document.querySelector(${JSON.stringify(futureCard + ' .scan-backup')})?.disabled === false`,
        ),
        'empty result does not disable task backup',
    )
    console.log(
        'PASS: no-match folder task is valid and scan states the empty result',
    )

    const socketSource = join(temporary, 'type-filter-socket')
    await mkdir(socketSource)
    await promisify(execFile)('python3', [
        fileURLToPath(new URL('./file_type_fixtures.py', import.meta.url)),
        socketSource,
        '1',
    ])
    const socketPath = join(socketSource, 'skip-0000.socket')
    await open()
    await click('.all-file-types input')
    await click('.special-type-toggle')
    await click('[data-file-type="socket"] input')
    await click('.choose-special-path')
    await click('[aria-label="特殊节点绝对路径"]')
    await page.call('Input.insertText', { text: socketPath })
    await click('[aria-label="添加路径"]')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.create-task')?.disabled === false",
            ),
        'direct socket ready',
    )
    assert(
        (
            await page.evaluate(
                "document.querySelector('.source-list').textContent",
            )
        ).includes('套接字'),
    )
    await capture('socket-composer')
    await click('.create-task')
    await until(
        () => page.evaluate("!document.querySelector('dialog[open]')"),
        'socket task saved',
    )
    const socketTask = (
        await page.evaluate('window.backup.getConfig()')
    ).tasks.find((task) =>
        task.selection?.some((item) => item.path === 'skip-0000.socket'),
    )
    assert.deepEqual(socketTask?.file_types, ['socket'])
    const socketCard = `.task-card[data-task-id="${socketTask.id}"]`
    await click(`${socketCard} .scan-button`)
    await until(
        () =>
            page.evaluate(
                `document.querySelector(${JSON.stringify(socketCard + ' .scan-table tbody tr')})?.dataset.type === 'socket'`,
            ),
        'socket scan preview',
    )
    console.log(
        'PASS: direct socket path can be selected and scanned through the task composer',
    )
}
