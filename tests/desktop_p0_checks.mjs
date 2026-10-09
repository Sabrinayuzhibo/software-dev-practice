import assert from 'node:assert/strict'
import { chmod, readFile, rm, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { setTimeout as delay } from 'node:timers/promises'

export async function checkP0({
    page,
    main,
    click,
    refresh,
    until,
    temporary,
    sourceDirectory,
    backupId,
}) {
    const trackClicks = () =>
        page.evaluate(`
        globalThis.windowTestClicks = [];
        document.addEventListener('click', event => {
            const button = event.target.closest('button');
            if (button) windowTestClicks.push({
                label: button.getAttribute('aria-label') || button.className,
                classes: [...button.classList],
                trusted: event.isTrusted,
            });
        }, true);
    `)
    await trackClicks()
    // Fail even the first poll so recovery cannot depend on a loaded record.
    await main.evaluate(`
        globalThis.failStatus = true;
        globalThis.failedStatusCalls = 0;
        globalThis.lostOperationId = null;
        scanAgentPrototype.send = async function(command, ...args) {
            if (command === 'operation' && args[0].id === lostOperationId && failStatus) {
                failedStatusCalls++;
                throw new Error('Status connection unavailable');
            }
            const result = await originalAgentSend.call(this, command, ...args);
            if (command === 'start_scan') lostOperationId = result.operation_id;
            if (command === 'operations' && globalThis.reloadLostStatus) {
                return { ...result, operations: result.operations.map(item =>
                    item.id === lostOperationId ? { ...item, state: 'RUNNING' } : item) };
            }
            return result;
        };
    `)
    await click('.scan-button')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.task-scan-progress h3')?.textContent.includes('状态失联')",
            ),
        'explicit lost state after three failed polls',
        10000,
    )
    await delay(2200)
    assert.equal(await main.evaluate('failedStatusCalls'), 3)
    assert(
        await page.evaluate(
            "document.querySelector('.primary-button').disabled",
        ),
    )
    assert(
        await page.evaluate(
            "!document.querySelector('.task-scan-progress progress')",
        ),
    )
    await main.evaluate(
        'globalThis.reloadLostStatus = true; failedStatusCalls = 0',
    )
    await page.call('Page.reload')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.task-scan-progress h3')?.textContent.includes('状态失联')",
            ),
        'reload retains ownership before the first successful status query',
        10000,
    )
    await trackClicks()
    await click('[data-tab="targets"]')
    assert(
        await page.evaluate("!document.querySelector('tbody button').disabled"),
    )
    await click('tbody button')
    await page.evaluate(`(() => {
        const input = document.querySelector('.target-form input');
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(input, 'Next run repository');
        input.dispatchEvent(new Event('input', { bubbles: true }));
    })()`)
    await click('.form-actions .backup-button')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.message-banner')?.textContent.includes('下次执行')",
            ),
        'active operation configuration save',
    )
    assert.equal(
        (await page.evaluate('window.backup.getConfig()')).targets[0].name,
        'Next run repository',
    )
    await main.evaluate('testWindow.setSize(850, 600)')
    await delay(150)
    assert(
        await page.evaluate(
            "document.documentElement.scrollWidth <= innerWidth && document.querySelector('.target-form').scrollWidth <= document.querySelector('.target-form').clientWidth",
        ),
    )
    let capture = await page.call('Page.captureScreenshot', { format: 'png' })
    await writeFile(
        join(tmpdir(), 'backup-targets-p0.png'),
        Buffer.from(capture.data, 'base64'),
    )
    await click('[data-tab="tasks"]')
    capture = await page.call('Page.captureScreenshot', { format: 'png' })
    await writeFile(
        join(tmpdir(), 'backup-operation-lost.png'),
        Buffer.from(capture.data, 'base64'),
    )
    await until(
        () =>
            page.evaluate(
                "!!document.querySelector('.operation-connection button')",
            ),
        'lost operation retry action',
        10000,
    )
    await main.evaluate('failStatus = false; reloadLostStatus = false')
    await click('.operation-connection button')
    await until(
        () =>
            page.evaluate(
                "!!document.querySelector('.scan-section') && !document.querySelector('.target-selector select').disabled",
            ),
        'manual retry retrieves actual completed result',
    )
    await main.evaluate('scanAgentPrototype.send = originalAgentSend')
    console.log(
        'PASS: first-poll loss stops after three attempts, retains operation lock and recovers; target edits stay available',
    )

    const blocked = ['unreadable-a', 'unreadable-b'].map((name) =>
        join(sourceDirectory, name),
    )
    try {
        for (const path of blocked) {
            await writeFile(path, 'unreadable')
            await chmod(path, 0)
        }
        await click('.scan-button')
        await until(
            () =>
                page.evaluate(
                    "document.querySelector('.scan-section .metrics')?.textContent.includes('不完整')",
                ),
            'incomplete scan preview',
        )
        assert(
            await page.evaluate(
                "document.querySelector('.scan-backup').disabled",
            ),
        )
        assert(
            await page.evaluate(
                "!!document.querySelector('.scan-table [data-type=directory]')",
            ),
        )
        await click('.scan-warnings summary')
        await until(
            () =>
                page.evaluate(
                    "document.querySelectorAll('.scan-warnings .warning-list p').length === 2",
                ),
            'aggregated unreadable paths',
        )
        for (const [width, height] of [
            [1120, 760],
            [850, 600],
        ]) {
            await main.evaluate(`testWindow.setSize(${width}, ${height})`)
            await delay(150)
            assert(
                await page.evaluate(
                    "document.documentElement.scrollWidth <= innerWidth && document.querySelector('.scan-table').scrollWidth <= document.querySelector('.scan-table').parentElement.clientWidth",
                ),
            )
            capture = await page.call('Page.captureScreenshot', {
                format: 'png',
            })
            await writeFile(
                join(tmpdir(), `backup-incomplete-${width}.png`),
                Buffer.from(capture.data, 'base64'),
            )
        }
    } finally {
        for (const path of blocked) {
            await chmod(path, 0o600)
            await rm(path)
        }
    }
    console.log(
        'PASS: incomplete scans retain typed preview, aggregate two errors and disable preview backup at both widths',
    )

    const staging = join(temporary, 'repository/storage/staging')
    try {
        await chmod(staging, 0o500)
        await refresh()
        assert(
            await page.evaluate(
                "document.querySelector('.status-dot').classList.contains('storage-unavailable')",
            ),
        )
        assert(
            await page.evaluate(
                "document.querySelector('.task-actions .backup-button').disabled && !document.querySelector('.scan-button').disabled",
            ),
        )
        await click('[data-tab="versions"]')
        await until(
            () =>
                page.evaluate(
                    "document.querySelectorAll('.versions-table tbody tr').length === 1",
                ),
            'existing versions readable when storage cannot be written',
        )
    } finally {
        await chmod(staging, 0o700)
    }
    await refresh()
    assert(
        await page.evaluate(
            "document.querySelector('.status-dot').classList.contains('online')",
        ),
    )

    // Serve real legacy warning data through Server -> Agent -> IPC -> React.
    const summaryPath = join(
        temporary,
        'repository/storage/versions',
        backupId,
        'summary.json',
    )
    const original = await readFile(summaryPath, 'utf8')
    try {
        const summary = JSON.parse(original)
        summary.warnings = Array.from({ length: 201 }, (_, index) => ({
            path: `legacy-warning-${index}`,
            reason: 'Special file skipped in basic backup',
        }))
        await writeFile(summaryPath, JSON.stringify(summary))
        await click('.section-heading button')
        await until(
            () =>
                page.evaluate(
                    "document.querySelector('.versions-table .warning-details summary')?.textContent === '201 项警告'",
                ),
            'legacy version warning count',
        )
        assert(
            await page.evaluate(
                "document.querySelector('.versions-table').textContent.includes('范围：完整目录')",
            ),
        )
        await click('.versions-table .warning-details summary')
        await until(
            () =>
                page.evaluate(
                    "document.querySelectorAll('.versions-table .warning-list p').length === 100",
                ),
            'first version warning page',
        )
        await click('.versions-table .warning-pagination button')
        await until(
            () =>
                page.evaluate(
                    "document.querySelectorAll('.versions-table .warning-list p').length === 200",
                ),
            'second version warning page',
        )
        await click('.versions-table .warning-pagination button')
        await until(
            () =>
                page.evaluate(
                    "document.querySelectorAll('.versions-table .warning-list p').length === 201",
                ),
            'final version warning page',
        )
    } finally {
        await writeFile(summaryPath, original)
    }
    await click('[data-tab="tasks"]')
    console.log(
        'PASS: storage unavailable differs from disconnected; historical version warnings paginate and show saved range',
    )
}
