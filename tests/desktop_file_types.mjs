import assert from 'node:assert/strict'
import { execFile } from 'node:child_process'
import {
    mkdir,
    writeFile,
    link,
    symlink,
    lstat,
    readlink,
} from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { setTimeout as delay } from 'node:timers/promises'
import { promisify } from 'node:util'
import { createFolderTask } from './desktop_sources.mjs'

export async function checkFileTypes({ page, main, click, until, temporary }) {
    const source = join(temporary, 'special-types')
    const destination = join(temporary, 'special-restored')
    await mkdir(join(source, 'folder'), { recursive: true })
    await writeFile(join(source, 'payload file.txt'), 'one shared payload\n')
    await link(
        join(source, 'payload file.txt'),
        join(source, 'folder/hardlink'),
    )
    await symlink('payload file.txt', join(source, 'relative-link'))
    const longTarget = '../outside/' + 'long-target-'.repeat(24)
    await symlink(longTarget, join(source, 'external-link'))
    await promisify(execFile)('mkfifo', [join(source, 'pipe')])
    await click('[data-tab="tasks"]')
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [${JSON.stringify(source)}] })`,
    )
    await createFolderTask({ page, click, until })
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.task-card').length === 1",
            ),
        'special type task',
    )
    await click('.scan-button')
    await until(
        () =>
            page.evaluate(
                "!!document.querySelector('.scan-table [data-type=hardlink]') && !document.querySelector('.scan-backup').disabled",
            ),
        'special type scan',
    )
    const preview = await page.evaluate(`(() => {
        const section = document.querySelector('.scan-section');
        return { text: section.textContent, types: [...section.querySelectorAll('tbody tr')].map(row => row.dataset.type) };
    })()`)
    assert.deepEqual(
        new Set(preview.types),
        new Set(['file', 'directory', 'hardlink', 'symlink', 'fifo']),
    )
    for (const text of [
        '2 个软链接',
        '1 个硬链接',
        '1 个命名管道',
        '指向：payload file.txt',
        '同组文件：',
    ]) {
        assert(preview.text.includes(text), text)
    }

    async function capture(name, selector) {
        for (const [width, height] of [
            [1120, 760],
            [850, 600],
        ]) {
            await main.evaluate(`testWindow.setSize(${width}, ${height})`)
            await delay(150)
            await page.evaluate(
                "document.querySelector('.content').scrollTop = 0",
            )
            assert(
                await page.evaluate(
                    `document.documentElement.scrollWidth <= innerWidth && [...document.querySelectorAll(${JSON.stringify(selector)})].every(element => element.scrollWidth <= element.parentElement.clientWidth)`,
                ),
            )
            const image = await page.call('Page.captureScreenshot', {
                format: 'png',
            })
            await writeFile(
                join(tmpdir(), `backup-types-${name}-${width}.png`),
                Buffer.from(image.data, 'base64'),
            )
            if (name === 'preview') {
                await page.evaluate(
                    "document.querySelector('.scan-table').scrollIntoView({ block: 'start' })",
                )
                await delay(150)
                const rows = await page.call('Page.captureScreenshot', {
                    format: 'png',
                })
                await writeFile(
                    join(tmpdir(), `backup-types-preview-rows-${width}.png`),
                    Buffer.from(rows.data, 'base64'),
                )
            }
        }
    }

    await capture('preview', '.scan-table')
    await click('.scan-backup')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.task-card .operation-panel h3')?.textContent === '备份 · 成功'",
            ),
        'special type backup',
    )
    await click('.view-versions')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.versions-table tbody tr').length === 1",
            ),
        'special version filter',
    )
    const versionText = await page.evaluate(
        "document.querySelector('.versions-table').textContent",
    )
    assert(
        versionText.includes('2 个软链接') &&
            versionText.includes('1 个硬链接') &&
            versionText.includes('1 个命名管道'),
    )
    assert(versionText.includes('内容存储：19 B'))
    await capture('version', '.versions-table')
    await click('.versions-table tbody button')
    await until(
        () => page.evaluate("!!document.querySelector('dialog[open]')"),
        'special restore confirmation',
    )
    assert(
        (
            await page.evaluate(
                "document.querySelector('.restore-summary').textContent",
            )
        ).includes('2 个软链接'),
    )
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [${JSON.stringify(destination)}] })`,
    )
    await click('[aria-label="选择还原目录"]')
    await until(
        () =>
            page.evaluate(
                `document.querySelector('[aria-label="还原目录"]').value === ${JSON.stringify(destination)} && !document.querySelector('.confirm-restore').disabled`,
            ),
        'special restore destination ready',
    )
    await click('.confirm-restore')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.operation-panel h2')?.textContent === '还原 · 成功'",
            ),
        'special restore result',
    )
    assert.equal(
        (await lstat(join(destination, 'payload file.txt'))).ino,
        (await lstat(join(destination, 'folder/hardlink'))).ino,
    )
    assert.equal(await readlink(join(destination, 'external-link')), longTarget)
    assert((await lstat(join(destination, 'pipe'))).isFIFO())
    await click('.restore-details summary')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.restore-details')?.textContent.includes('已写入 6 项')",
            ),
        'special restore journal',
    )
    const journal = await page.evaluate(
        "document.querySelector('.restore-details').textContent",
    )
    assert(
        journal.includes('硬链接') &&
            journal.includes('软链接') &&
            journal.includes('命名管道'),
    )
    await capture('restore', '.versions-table')
    console.log(
        'PASS: FR-10 desktop scan, linked paths, version storage, restore confirmation, real inode/FIFO and typed journals at both widths',
    )
}
