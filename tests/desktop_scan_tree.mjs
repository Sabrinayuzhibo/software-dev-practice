import assert from 'node:assert/strict'
import { execFile } from 'node:child_process'
import { promisify } from 'node:util'
import { mkdir, writeFile, symlink } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { setTimeout as delay } from 'node:timers/promises'
import { createFolderTask } from './desktop_sources.mjs'

export async function checkScanTree({ page, main, click, until, temporary }) {
    const source = join(temporary, 'tree-preview-source')
    await mkdir(join(source, 'docs/deep'), { recursive: true })
    await mkdir(join(source, 'other'))
    await mkdir(join(source, 'empty'))
    for (const name of [
        'root.txt',
        'docs/a.txt',
        'docs/deep/a.txt',
        'other/a.txt',
    ]) {
        await writeFile(join(source, name), `content for ${name}`)
    }
    const longName = '中文 文件-' + 'long-name-'.repeat(18) + '.txt'
    await writeFile(join(source, longName), '')
    await symlink('docs', join(source, 'alias'))
    await promisify(execFile)('python3', [
        '-c',
        'import socket,sys; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.close()',
        join(source, 'skipped.socket'),
    ])
    await click('[data-tab="tasks"]')
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [${JSON.stringify(source)}] })`,
    )
    await createFolderTask({ page, click, until })
    const task = (await page.evaluate('window.backup.getConfig()')).tasks.find(
        (item) => item.path === source,
    )
    const card = `[data-task-id="${task.id}"]`
    const row = (path) => `${card} .scan-table tr[data-path="${path}"]`
    const exists = (path) =>
        page.evaluate(`!!document.querySelector(${JSON.stringify(row(path))})`)
    const toggle = (path) => click(`${row(path)} .scan-directory-toggle`)
    async function scan() {
        await click(`${card} .scan-button`)
        await until(
            () =>
                page.evaluate(
                    `document.querySelector(${JSON.stringify(card + ' .scan-backup')})?.disabled === false`,
                ),
            'tree scan finished',
        )
    }
    await scan()
    await click(`${card} .scan-warnings summary`)
    await until(
        () =>
            page.evaluate(
                `document.querySelectorAll(${JSON.stringify(card + ' .scan-warnings .warning-list p')}).length === 1`,
            ),
        'tree and warning list update independently',
    )
    assert(await exists('docs/deep'))
    assert.equal(await exists('docs/deep/a.txt'), false)
    await toggle('docs/deep')
    assert(await exists('docs/deep/a.txt'))
    await toggle('docs')
    assert.equal(await exists('docs/deep/a.txt'), false)
    await toggle('docs')
    assert(await exists('docs/deep/a.txt'))
    assert.equal(
        await page.evaluate(
            `document.querySelector(${JSON.stringify(row('alias'))}).querySelectorAll('button').length`,
        ),
        0,
    )
    await click(`${card} .scan-collapse-all`)
    assert.equal(await exists('docs/a.txt'), false)
    await click(`${card} .scan-expand-all`)
    assert(await exists('docs/a.txt'))
    assert(await exists('docs/deep/a.txt'))
    assert(await exists('other/a.txt'))
    assert.equal(
        await page.evaluate(
            `document.querySelector(${JSON.stringify(row('docs/deep/a.txt'))}).querySelector('.scan-tree-name').textContent`,
        ),
        'a.txt',
    )
    await click(`${row('docs/a.txt')} .digest-details summary`)
    assert.equal(
        (
            await page.evaluate(
                `document.querySelector(${JSON.stringify(row('docs/a.txt'))}).querySelector('.digest-details code').textContent`,
            )
        ).length,
        64,
    )
    for (const [width, height] of [
        [1120, 760],
        [850, 600],
    ]) {
        await main.evaluate(`testWindow.setSize(${width}, ${height})`)
        await delay(160)
        await page.evaluate(
            `document.querySelector(${JSON.stringify(card + ' .scan-table')}).scrollIntoView({block:'start'})`,
        )
        assert(
            await page.evaluate(`(() => {
            const table = document.querySelector(${JSON.stringify(card + ' .scan-table')});
            return document.documentElement.scrollWidth <= innerWidth && table.scrollWidth <= table.parentElement.clientWidth;
        })()`),
            'tree fits window including long Chinese names',
        )
        const capture = await page.call('Page.captureScreenshot', {
            format: 'png',
        })
        await writeFile(
            join(tmpdir(), `backup-scan-tree-${width}.png`),
            Buffer.from(capture.data, 'base64'),
        )
    }
    await main.evaluate('testWindow.setSize(1120, 760)')
    await click(`${card} .scan-collapse-all`)
    await scan()
    assert(await exists('docs/a.txt'))
    assert.equal(await exists('docs/deep/a.txt'), false)
    await page.evaluate(`window.backup.removeTask('${task.id}')`)
    await page.call('Page.reload')
    await until(
        () => page.evaluate("!!document.querySelector('.primary-button')"),
        'tree cleanup',
    )
    console.log(
        'PASS: real scan tree, nested toggles, retained child state, bulk actions, symlink leaf, warnings, digest, rescan and two layouts',
    )
}
