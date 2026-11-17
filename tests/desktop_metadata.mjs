import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { mkdir, readFile, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { setTimeout as delay } from 'node:timers/promises'
import { createFolderTask } from './desktop_sources.mjs'

export async function checkMetadata({ page, main, click, until, temporary }) {
    const source = join(temporary, 'metadata-gui-source')
    await mkdir(source)
    await writeFile(join(source, 'file.txt'), 'metadata GUI round trip')
    await click('[data-tab="tasks"]')
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [${JSON.stringify(source)}] })`,
    )
    await createFolderTask({ page, click, until })
    const task = (await page.evaluate('window.backup.getConfig()')).tasks.find(
        (item) => item.path === source,
    )
    const started = await page.evaluate(
        `window.backup.backupTask('${task.id}')`,
    )
    let backup
    await until(async () => {
        backup = await page.evaluate(
            `window.backup.getOperation('${started.operation_id}')`,
        )
        return backup.state !== 'RUNNING'
    }, 'metadata GUI backup')
    assert.equal(backup.state, 'SUCCEEDED')
    const folder = join(
        temporary,
        'repository/storage/versions',
        backup.result.version.id,
    )
    const summary = JSON.parse(
        await readFile(join(folder, 'summary.json'), 'utf8'),
    )
    const entries = (await readFile(join(folder, 'entries.jsonl'), 'utf8'))
        .trimEnd()
        .split('\n')
        .map(JSON.parse)
    // 模拟来自另一属主的合法版本；UID 合法，但当前用户无权恢复该属主。
    const foreignUid = String(process.getuid() + 1)
    for (const entry of entries) {
        entry.uid = foreignUid
    }
    summary.root_metadata.uid = foreignUid
    const manifest = entries
        .map((entry) => JSON.stringify(entry) + '\n')
        .join('')
    summary.manifest_sha256 = createHash('sha256')
        .update(manifest)
        .digest('hex')
    await writeFile(join(folder, 'entries.jsonl'), manifest)
    await writeFile(join(folder, 'summary.json'), JSON.stringify(summary))
    await click(`[data-task-id="${task.id}"] .task-versions`)
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.versions-table tbody tr').length === 1",
            ),
        'metadata GUI version',
    )
    await click('.versions-table tbody .version-actions button')
    await until(
        () => page.evaluate("!!document.querySelector('.restore-summary')"),
        'metadata restore confirmation',
    )
    const destination = join(temporary, 'metadata-gui-restored')
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [${JSON.stringify(destination)}] })`,
    )
    await click('[aria-label="选择还原目录"]')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.confirm-restore')?.disabled === false",
            ),
        'metadata restore target',
    )
    await click('.confirm-restore')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.metadata-warning')?.textContent.includes('内容已恢复，元数据未完全恢复')",
            ),
        'metadata warning banner',
    )
    assert.equal(
        await readFile(join(destination, 'file.txt'), 'utf8'),
        'metadata GUI round trip',
    )
    for (const [width, height] of [
        [1120, 760],
        [850, 600],
    ]) {
        await main.evaluate(`testWindow.setSize(${width}, ${height})`)
        await delay(160)
        await page.evaluate(
            "document.querySelector('.metadata-warning').scrollIntoView({block:'center'})",
        )
        const layout = await page.evaluate(`(() => {
            const panel = document.querySelector('.operation-panel');
            const edge = panel.getBoundingClientRect();
            const banner = panel.querySelector('.metadata-warning');
            const content = [...panel.querySelectorAll('button,p,details,summary,input,select')];
            return {document:document.documentElement.scrollWidth, viewport:innerWidth,
                textFits:banner.scrollWidth <= banner.clientWidth + 1,
                contentFits:content.every(item => {
                    const rect = item.getBoundingClientRect();
                    return !rect.width || (rect.left >= edge.left - 1 && rect.right <= edge.right + 1);
                })};
        })()`)
        const capture = await page.call('Page.captureScreenshot', {
            format: 'png',
        })
        await writeFile(
            join(tmpdir(), `backup-metadata-${width}.png`),
            Buffer.from(capture.data, 'base64'),
        )
        assert(
            layout.document <= layout.viewport &&
                layout.textFits &&
                layout.contentFits,
            'metadata result fits narrow window: ' + JSON.stringify(layout),
        )
    }
    await main.evaluate('testWindow.setSize(1120, 760)')
    await click('.operation-panel .warning-details summary')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.operation-panel .warning-list p').length === 2",
            ),
        'root and file ownership warnings',
    )
    await click('.restore-details summary')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.restore-details').textContent.includes('已写入 1 项')",
            ),
        'metadata warnings coexist with restore journal',
    )
    const id = await page.evaluate(
        "document.querySelector('.operation-details small').textContent.replace('执行编号：', '').trim()",
    )
    await page.call('Page.reload')
    await until(
        () => page.evaluate("!!document.querySelector('[data-tab=records]')"),
        'metadata reload',
    )
    await click('[data-tab="records"]')
    await click(`[data-operation-id="${id}"] .record-view`)
    await until(
        () => page.evaluate("!!document.querySelector('.metadata-warning')"),
        'persisted metadata warning banner',
    )
    await click('.operation-panel .warning-details summary')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.operation-panel .warning-list p').length === 2",
            ),
        'persisted metadata warning details',
    )
    await page.evaluate(`window.backup.removeTask('${task.id}')`)
    await page.call('Page.reload')
    await until(
        () => page.evaluate("!!document.querySelector('.primary-button')"),
        'metadata cleanup',
    )
    console.log(
        'PASS: metadata partial success banner, real ownership failure, content restore, warning/journal independent toggles and reload',
    )
}
