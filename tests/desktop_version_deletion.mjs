import assert from 'node:assert/strict'
import { chmod, mkdir, readFile, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { setTimeout as delay } from 'node:timers/promises'

export async function checkVersionDeletion({
    page,
    main,
    click,
    until,
    temporary,
}) {
    const source = join(temporary, 'delete-version-source')
    await mkdir(source)
    await writeFile(
        join(source, 'keep.txt'),
        'keep the source and other version',
    )
    const task = await page.evaluate(`window.backup.createTask(
        [${JSON.stringify(source)}], 'local', '删除测试', ['file'], true
    )`)
    const versions = []
    for (let index = 0; index < 2; index++) {
        const operation = await page.evaluate(
            `window.backup.backupTask('${task.id}')`,
        )
        await until(
            async () => {
                const record = await page.evaluate(
                    `window.backup.getOperation('${operation.operation_id}')`,
                )
                if (record.state === 'RUNNING') {
                    return false
                }
                assert.equal(record.state, 'SUCCEEDED')
                return true
            },
            'deletion fixture backup',
            20000,
        )
        versions.push(operation.operation_id)
    }
    await page.call('Page.reload')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.status-dot')?.classList.contains('online')",
            ),
        'reloaded deletion target',
    )
    await click('[data-tab="versions"]')
    const row = `[data-version-id="${versions[0]}"]`
    await until(
        () =>
            page.evaluate(
                `document.querySelector('${row} .delete-version')?.disabled === false`,
            ),
        'version deletion entry',
    )
    await click(`${row} .delete-version`)
    await until(
        () =>
            page.evaluate(
                "!!document.querySelector('.confirm-delete-version')",
            ),
        'deletion confirmation',
    )
    const summary = await page.evaluate(
        "document.querySelector('dialog').textContent",
    )
    assert(summary.includes(versions[0]) && summary.includes(source))
    assert(summary.includes('无法撤销') && summary.includes('备份时间'))
    assert.equal(
        await page.evaluate('document.activeElement.textContent.trim()'),
        '取消',
    )
    for (const [width, height] of [
        [1120, 760],
        [850, 600],
    ]) {
        await main.evaluate(`testWindow.setSize(${width}, ${height})`)
        await delay(200)
        assert(
            await page.evaluate(`(() => {
            const dialog = document.querySelector('dialog');
            const button = dialog.querySelector('.confirm-delete-version');
            const bounds = dialog.getBoundingClientRect();
            return getComputedStyle(dialog).backgroundColor === 'rgb(255, 255, 255)' &&
                !dialog.querySelector(':scope > canvas') &&
                button.querySelector('canvas') &&
                button.getBoundingClientRect().bottom <= bounds.bottom &&
                dialog.scrollWidth <= dialog.clientWidth &&
                document.documentElement.scrollWidth <= innerWidth;
        })()`),
        )
        const capture = await page.call('Page.captureScreenshot', {
            format: 'png',
        })
        await writeFile(
            join(tmpdir(), `backup-delete-confirm-${width}.png`),
            Buffer.from(capture.data, 'base64'),
        )
    }
    await click('.dialog-actions button:first-child')
    assert(
        (
            await page.evaluate("window.backup.listVersions('local')")
        ).versions.some((item) => item.id === versions[0]),
    )

    // A start failure must leave the confirmation open with a usable retry.
    await main.evaluate(`
        globalThis.deleteBridge = process.mainModule.require(testElectron.app.getAppPath() + '/dist-electron/agent-bridge.js').AgentBridge.prototype;
        globalThis.beforeDeleteSend = deleteBridge.send;
        globalThis.failDeleteStart = true;
        deleteBridge.send = function(action, ...args) {
            if (action === 'start_delete_version' && failDeleteStart) {
                return Promise.reject(new Error('Deletion start unavailable'));
            }
            return beforeDeleteSend.call(this, action, ...args);
        };
    `)
    await click(`${row} .delete-version`)
    await click('.confirm-delete-version')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('dialog [role=alert]')?.textContent.includes('Deletion start unavailable')",
            ),
        'delete start failure',
    )
    await main.evaluate('failDeleteStart = false')
    const receipts = join(temporary, 'repository/database/deletions')
    await chmod(receipts, 0o500)
    try {
        await click('.confirm-delete-version')
        await until(
            () =>
                page.evaluate(
                    "document.querySelector('.operation-panel h2')?.textContent === '删除版本 · 失败'",
                ),
            'persisted deletion failure',
        )
    } finally {
        await chmod(receipts, 0o700)
    }
    await click('[data-tab="records"]')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.records-table')?.textContent.includes('删除版本')",
            ),
        'failed deletion record',
    )
    await click('.records-table tbody tr:first-child button')
    await until(
        () =>
            page.evaluate("!!document.querySelector('.retry-delete-version')"),
        'deletion retry entry',
    )
    await click('.retry-delete-version')
    assert(
        (
            await page.evaluate("document.querySelector('dialog').textContent")
        ).includes(versions[0]),
    )
    await click('.confirm-delete-version')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.operation-panel h2')?.textContent === '删除版本 · 成功'",
            ),
        'delete version success',
        15000,
    )
    await click('[data-tab="versions"]')
    await until(
        () =>
            page.evaluate(
                `!document.querySelector('${row}') && !!document.querySelector('[data-version-id="${versions[1]}"]')`,
            ),
        'list refreshed after deletion',
    )
    await main.evaluate('deleteBridge.send = beforeDeleteSend')
    assert.equal(
        await readFile(join(source, 'keep.txt'), 'utf8'),
        'keep the source and other version',
    )
    assert(
        (await page.evaluate('window.backup.getConfig()')).tasks.some(
            (item) => item.id === task.id,
        ),
    )
    await click('[data-tab="records"]')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.records-table')?.textContent.includes('删除版本')",
            ),
        'delete execution record',
    )
    await page.call('Page.reload')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.status-dot')?.classList.contains('online')",
            ),
        'delete restart',
    )
    const records = await page.evaluate('window.backup.listOperations()')
    assert(
        records.operations.some(
            (item) =>
                item.action === 'delete_version' &&
                item.version_id === versions[0] &&
                item.state === 'SUCCEEDED',
        ),
    )
    const restore = await page.evaluate(
        `window.backup.restoreVersion('local', '${versions[1]}', ${JSON.stringify(join(temporary, 'after-delete-restore'))})`,
    )
    await until(async () => {
        const record = await page.evaluate(
            `window.backup.getOperation('${restore.operation_id}')`,
        )
        if (record.state === 'RUNNING') {
            return false
        }
        assert.equal(record.state, 'SUCCEEDED')
        return true
    }, 'remaining version restore')
    assert.equal(
        await readFile(
            join(temporary, 'after-delete-restore/keep.txt'),
            'utf8',
        ),
        'keep the source and other version',
    )
    console.log(
        'PASS: deletion confirmation/cancel/retry, solid dialog and glass buttons, list refresh, persisted history, surviving version restore',
    )
}
