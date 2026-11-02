import assert from 'node:assert/strict'
import { mkdir, readFile, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { setTimeout as delay } from 'node:timers/promises'

export async function checkCompression({
    page,
    main,
    click,
    until,
    temporary,
}) {
    const source = join(temporary, 'compression-source')
    await mkdir(source)
    const contents = '压缩备份 abcabcabc\n'.repeat(30000)
    await writeFile(join(source, '中文.txt'), contents)
    const task = await page.evaluate(`window.backup.createTask(
        [${JSON.stringify(source)}], 'local', '压缩测试', ['file'], true
    )`)
    for (const algorithm of ['huffman', 'lz77']) {
        await page.call('Page.reload')
        await until(
            () =>
                page.evaluate(
                    "document.querySelector('.status-dot')?.classList.contains('online')",
                ),
            'compression target connected',
            15000,
        )
        await click('[data-tab="targets"]')
        await click('tbody tr:first-child button')
        await page.evaluate(`(() => {
            const select = document.querySelector('[aria-label="备份压缩算法"]');
            select.value = '${algorithm}';
            select.dispatchEvent(new Event('change', { bubbles: true }));
        })()`)
        await click('.target-form .backup-button')
        await until(async () => {
            const config = await page.evaluate('window.backup.getConfig()')
            return (
                config.targets.find((target) => target.id === 'local')
                    ?.compression === algorithm
            )
        }, 'saved compression policy')
        await until(
            () =>
                page.evaluate(
                    "document.querySelector('.target-form h2')?.textContent === '新建目标' && !document.querySelector('.target-form .backup-button').disabled",
                ),
            'compression form reset after save',
        )
        // 保存后表单会重置为新建目标，需重新打开已保存的目标。
        await click('tbody tr:first-child button')
        await until(
            () =>
                page.evaluate(
                    `document.querySelector('[aria-label="备份压缩算法"]').value === '${algorithm}'`,
                ),
            'reopened compression policy',
        )
        for (const [width, height] of [
            [1120, 760],
            [850, 600],
        ]) {
            await main.evaluate(`testWindow.setSize(${width}, ${height})`)
            await page.evaluate(
                "document.querySelector('[aria-label=\"备份压缩算法\"]').scrollIntoView({ block: 'center' })",
            )
            await delay(150)
            const layout = await page.evaluate(`(() => {
                const select = document.querySelector('[aria-label="备份压缩算法"]');
                return {
                    selected: select.value,
                    hint: document.querySelector('.compression-policy-hint').textContent,
                    scrollWidth: document.documentElement.scrollWidth,
                    width: innerWidth,
                };
            })()`)
            assert.equal(layout.selected, algorithm, JSON.stringify(layout))
            assert(layout.hint.includes('已有版本'), JSON.stringify(layout))
            assert(layout.scrollWidth <= layout.width, JSON.stringify(layout))
            const capture = await page.call('Page.captureScreenshot', {
                format: 'png',
            })
            await writeFile(
                join(tmpdir(), `backup-compression-${algorithm}-${width}.png`),
                Buffer.from(capture.data, 'base64'),
            )
        }
        const backup = await page.evaluate(
            `window.backup.backupTask('${task.id}')`,
        )
        await until(
            async () => {
                const operation = await page.evaluate(
                    `window.backup.getOperation('${backup.operation_id}')`,
                )
                if (operation.state === 'RUNNING') {
                    return false
                }
                assert.equal(operation.state, 'SUCCEEDED')
                assert.equal(operation.result.version.compression, algorithm)
                assert(
                    Number(operation.result.version.stored_bytes) <
                        Buffer.byteLength(contents),
                )
                return true
            },
            'compressed backup',
            20000,
        )
        await page.call('Page.reload')
        await until(
            () =>
                page.evaluate(
                    "document.querySelector('.status-dot')?.classList.contains('online')",
                ),
            'compression reload',
            15000,
        )
        await click('[data-tab="versions"]')
        await until(
            () =>
                page.evaluate(
                    `document.querySelector('[data-version-id="${backup.operation_id}"] .version-compression')?.textContent === '${algorithm === 'huffman' ? 'Huffman' : 'LZ77'}'`,
                ),
            'saved version compression label',
        )
        const destination = join(temporary, `compression-restored-${algorithm}`)
        const restore = await page.evaluate(
            `window.backup.restoreVersion('local', '${backup.operation_id}', ${JSON.stringify(destination)})`,
        )
        await until(
            async () => {
                const operation = await page.evaluate(
                    `window.backup.getOperation('${restore.operation_id}')`,
                )
                if (operation.state === 'RUNNING') {
                    return false
                }
                assert.equal(operation.state, 'SUCCEEDED')
                return true
            },
            'decompressed restore',
            20000,
        )
        assert.equal(
            await readFile(join(destination, '中文.txt'), 'utf8'),
            contents,
        )
    }
    // 恢复默认压缩策略，供后续窗口恢复检查使用。
    const target = (
        await page.evaluate('window.backup.getConfig()')
    ).targets.find((item) => item.id === 'local')
    await page.evaluate(
        `window.backup.saveTarget(${JSON.stringify({ ...target, compression: 'none' })})`,
    )
    console.log(
        'PASS: compression policy selection/persistence, two window sizes, actual Huffman/LZ77 backups and automatic restore',
    )
}
