import assert from 'node:assert/strict'
import { mkdir, readFile, readdir, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { setTimeout as delay } from 'node:timers/promises'

export async function checkCustomFilters({
    page,
    main,
    click,
    until,
    temporary,
}) {
    const source = join(temporary, 'custom-filter-source')
    await mkdir(join(source, 'sub'), { recursive: true })
    await writeFile(join(source, 'sub', 'a.txt'), 'a'.repeat(10))
    await writeFile(join(source, 'sub', 'b.txt'), 'b'.repeat(20))
    await writeFile(join(source, 'sub', 'skip.log'), 'x'.repeat(15))
    const now = Math.floor(Date.now() / 1000)
    const local = (seconds) => {
        const date = new Date(seconds * 1000)
        return new Date(date.getTime() - date.getTimezoneOffset() * 60000)
            .toISOString()
            .slice(0, 19)
    }
    async function set(selector, value) {
        await page.evaluate(`(() => {
            const element = document.querySelector(${JSON.stringify(selector)});
            const prototype = element.tagName === 'SELECT' ? HTMLSelectElement.prototype : HTMLInputElement.prototype;
            Object.getOwnPropertyDescriptor(prototype, 'value').set.call(element, ${JSON.stringify(value)});
            element.dispatchEvent(new Event('input', { bubbles: true }));
            element.dispatchEvent(new Event('change', { bubbles: true }));
        })()`)
        await delay(70)
    }
    async function add(category, fields, polarity = 'include') {
        await click(`#filter-tab-${category}`)
        await click(`.add-filter-${polarity}`)
        for (const [label, value] of Object.entries(fields)) {
            await set(
                `[data-polarity="${polarity}"] [aria-label="${label}"]`,
                value,
            )
        }
    }
    async function waitOperation(id) {
        let result
        await until(
            async () => {
                result = await page.evaluate(
                    `window.backup.getOperation('${id}')`,
                )
                if (result.state === 'RUNNING') {
                    return false
                }
                assert(
                    result.state.startsWith('SUCCEEDED'),
                    JSON.stringify(result),
                )
                return true
            },
            'custom filter operation',
            15000,
        )
        return result
    }
    await click('[data-tab="tasks"]')
    await click('.primary-button')
    await until(
        () => page.evaluate("!!document.querySelector('.filter-editor')"),
        'custom filter editor',
    )
    // 复现跨页签的空条件：提示必须指出实际出错的类别。
    await click('#filter-tab-name')
    await click('.add-filter-include')
    await click('#filter-tab-size')
    await click('.add-filter-include')
    await click('#filter-tab-path')
    const invalidMessage = await page.evaluate(
        "document.querySelector('.filter-editor [role=alert]').textContent",
    )
    assert.match(invalidMessage, /名字.*第 1 条/)
    assert.equal(
        await page.evaluate("document.querySelector('.create-task').disabled"),
        true,
    )
    await click('.locate-filter-error')
    assert.equal(
        await page.evaluate(
            "document.querySelector('.filter-tabs [aria-selected=true]').id",
        ),
        'filter-tab-name',
    )
    await click('.remove-filter')
    assert.match(
        await page.evaluate(
            "document.querySelector('.filter-editor [role=alert]').textContent",
        ),
        /尺寸.*第 1 条/,
    )
    await click('.locate-filter-error')
    await click('.remove-filter')
    assert.equal(
        await page.evaluate(
            "document.querySelector('.filter-editor [role=alert]') === null",
        ),
        true,
    )
    await add('path', { 路径条件: 'sub/' })
    await add('type', { 匹配类型: 'file' })
    await add('name', { 名字条件: '*.txt' })
    await add('size', { 尺寸下限: '10', 尺寸上限: '20' })
    await add('mtime', { 时间下限: local(now - 60), 时间上限: local(now + 60) })
    await add('user', { 用户条件: String(process.getuid()) })
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [${JSON.stringify(source)}] })`,
    )
    await click('.choose-folders')
    await until(
        () => page.evaluate("!document.querySelector('.create-task').disabled"),
        'filtered source plan',
    )
    assert.equal(
        await page.evaluate(
            "document.querySelector('.filter-summary').textContent.includes('选入 6 条')",
        ),
        true,
    )
    await click('.create-task')
    await until(
        () => page.evaluate("!document.querySelector('dialog[open]')"),
        'filtered task saved',
    )
    const config = await page.evaluate('window.backup.getConfig()')
    const task = config.tasks.find((item) => item.path === source)
    assert.equal(task.filters.include.length, 6)
    const card = `[data-task-id="${task.id}"]`
    await click(`${card} .edit-task-filters`)
    await until(
        () => page.evaluate("!!document.querySelector('.filter-dialog-body')"),
        'edit saved rules',
    )
    await add('name', { 名字条件: 'b.txt' }, 'exclude')
    await click('.preview-filters')
    await until(
        () => page.evaluate("!!document.querySelector('.filter-preview')"),
        'unsaved rules preview',
        15000,
    )
    assert.match(
        await page.evaluate(
            "document.querySelector('.filter-preview').textContent",
        ),
        /1 个普通文件/,
    )
    const draftConfig = await page.evaluate('window.backup.getConfig()')
    assert.equal(
        draftConfig.tasks.find((item) => item.id === task.id).filters.exclude
            .length,
        0,
    )
    for (const [width, height] of [
        [1120, 760],
        [850, 600],
    ]) {
        await main.evaluate(`testWindow.setSize(${width}, ${height})`)
        await delay(160)
        assert(
            await page.evaluate(`(() => {
            const dialog = document.querySelector('dialog[open]');
            const footer = dialog.querySelector('.dialog-actions').getBoundingClientRect();
            return document.documentElement.scrollWidth <= innerWidth &&
                dialog.scrollWidth <= dialog.clientWidth && footer.bottom <= dialog.getBoundingClientRect().bottom;
        })()`),
            'custom filters fit and actions remain visible',
        )
        const capture = await page.call('Page.captureScreenshot', {
            format: 'png',
        })
        await writeFile(
            join(tmpdir(), `backup-custom-filter-${width}.png`),
            Buffer.from(capture.data, 'base64'),
        )
    }
    await click('.save-filters')
    await until(
        () => page.evaluate("!document.querySelector('dialog[open]')"),
        'rules saved',
    )
    await page.call('Page.reload')
    await until(
        () =>
            page.evaluate(
                `!!document.querySelector('${card} .edit-task-filters')`,
            ),
        'rules reload',
        15000,
    )
    await click(`${card} .edit-task-filters`)
    for (const [category, label, invalid, valid] of [
        ['path', '路径条件', '../escape', 'sub/'],
        ['path', '路径条件', '/absolute', 'sub/'],
        ['name', '名字条件', 'nested/a.txt', '*.txt'],
        ['size', '尺寸下限', '-1', '10'],
        ['size', '尺寸下限', '21', '10'],
        ['user', '用户条件', '4294967296', String(process.getuid())],
        ['user', '用户条件', 'user-name', String(process.getuid())],
        ['mtime', '时间下限', local(now + 120), local(now - 60)],
    ]) {
        await click(`#filter-tab-${category}`)
        await set(`[data-polarity="include"] [aria-label="${label}"]`, invalid)
        assert.equal(
            await page.evaluate(
                "document.querySelector('.save-filters').disabled",
            ),
            true,
            `${label} rejects ${invalid}`,
        )
        assert(
            await page.evaluate(
                "document.querySelector('.filter-editor [role=alert]').textContent.includes('第 1 条')",
            ),
        )
        await set(`[data-polarity="include"] [aria-label="${label}"]`, valid)
        assert.equal(
            await page.evaluate(
                "document.querySelector('.save-filters').disabled",
            ),
            false,
            `${label} recovers after correction`,
        )
    }
    await click('#filter-tab-size')
    await set('[data-polarity="include"] [aria-label="尺寸下限"]', '-1')
    await click('[aria-label="关闭对话框"]')
    const saved = await page.evaluate('window.backup.getConfig()')
    assert.equal(
        saved.tasks
            .find((item) => item.id === task.id)
            .filters.include.find((item) => item.category === 'size').min,
        '10',
    )
    const backup = await page.evaluate(`window.backup.backupTask('${task.id}')`)
    const backed = await waitOperation(backup.operation_id)
    assert.equal(backed.result.version.files, '1')
    assert.equal(backed.result.version.filters.exclude[0].pattern, 'b.txt')
    const destination = join(temporary, 'custom-filter-output')
    const restore = await page.evaluate(
        `window.backup.restoreVersion('local', '${backed.result.version.id}', ${JSON.stringify(destination)})`,
    )
    await waitOperation(restore.operation_id)
    assert.deepEqual(await readdir(join(destination, 'sub')), ['a.txt'])
    assert.equal(
        await readFile(join(destination, 'sub', 'a.txt'), 'utf8'),
        'a'.repeat(10),
    )
    await main.evaluate('testWindow.setSize(1120, 760)')
    await page.evaluate(`window.backup.removeTask('${task.id}')`)
    await page.call('Page.reload')
    await until(
        () => page.evaluate("!!document.querySelector('.primary-button')"),
        'filter cleanup reload',
        15000,
    )
    console.log(
        'PASS: six filter controls, cross-tab errors, 8 invalid inputs, draft preview, persistence, 2 layouts and actual backup/restore',
    )
}
