// Runs the actual Electron window. Requires the same display session as run-agent.sh.
// Mouse events go through Chromium input routing, not HTMLElement.click().
import assert from 'node:assert/strict'
import { spawn, execFile } from 'node:child_process'
import { promisify } from 'node:util'
import {
    mkdtemp,
    mkdir,
    readFile,
    writeFile,
    readdir,
    symlink,
    rm,
} from 'node:fs/promises'
import { createServer } from 'node:net'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { setTimeout as delay } from 'node:timers/promises'
import { checkP0 } from './desktop_p0_checks.mjs'

const root = fileURLToPath(new URL('../', import.meta.url))
const execute = promisify(execFile)
const nativeInput = process.env.BACKUP_TEST_WINDOWS_INPUT === '1'
const testTitle = `Backup Window Regression ${process.pid}`

async function windowsInput(action, point = {}) {
    const { stdout: script } = await execute('wslpath', [
        '-w',
        join(root, 'tests/windows_window_input.ps1'),
    ])
    const { stdout } = await execute(
        'powershell.exe',
        [
            '-NoProfile',
            '-NonInteractive',
            '-ExecutionPolicy',
            'Bypass',
            '-File',
            script.trim(),
            '-Title',
            testTitle,
            '-Action',
            action,
            '-X',
            String(point.x || 0),
            '-Y',
            String(point.y || 0),
        ],
        { timeout: 10000 },
    )
    return JSON.parse(stdout)
}
const temporary = await mkdtemp(join(tmpdir(), 'backup-window-test-'))
const sourceDirectory = join(temporary, 'source')
const secondSourceDirectory = join(temporary, 'another', 'source')
const restoreDirectory = join(temporary, 'restored')
await mkdir(join(sourceDirectory, 'empty'), { recursive: true })
await mkdir(secondSourceDirectory, { recursive: true })
await writeFile(join(secondSourceDirectory, 'second.txt'), 'second task\n')
await symlink('second.txt', join(secondSourceDirectory, 'skip.link'))
for (let index = 0; index < 200; index++) {
    await symlink(
        'second.txt',
        join(secondSourceDirectory, `skip-${index}.link`),
    )
}
await mkdir(restoreDirectory)
await writeFile(
    join(sourceDirectory, '中文 file.txt'),
    'desktop backup roundtrip\n',
)
const server = spawn(join(root, 'scripts/run-server.sh'), ['--port', '0'], {
    cwd: root,
    env: { ...process.env, BACKUP_DATA_DIR: join(temporary, 'repository') },
    stdio: ['ignore', 'pipe', 'pipe'],
})
let serverOutput = ''
server.stdout.on('data', (data) => {
    serverOutput += data
})
server.stderr.on('data', (data) => {
    serverOutput += data
})
const slowConnections = new Set()
const slowServer = createServer((socket) => {
    slowConnections.add(socket)
    socket.once('close', () => slowConnections.delete(socket))
})
await new Promise((resolve) => slowServer.listen(0, '127.0.0.1', resolve))

async function freePort() {
    const server = createServer()
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve))
    const port = server.address().port
    await new Promise((resolve) => server.close(resolve))
    return port
}

async function until(check, label, timeout = 6000) {
    const deadline = Date.now() + timeout
    while (Date.now() < deadline) {
        if (await check()) return
        await delay(80)
    }
    throw new Error('Timed out: ' + label)
}

async function connect(port) {
    let url
    await until(
        async () => {
            try {
                const targets = await (
                    await fetch(`http://127.0.0.1:${port}/json/list`)
                ).json()
                url = targets[0]?.webSocketDebuggerUrl
                return !!url
            } catch {
                return false
            }
        },
        'Electron debugger startup',
        30000,
    )
    const socket = new WebSocket(url)
    await new Promise((resolve, reject) => {
        socket.onopen = resolve
        socket.onerror = reject
    })
    let nextId = 0
    const pending = new Map()
    socket.onmessage = ({ data }) => {
        const message = JSON.parse(data)
        const request = pending.get(message.id)
        if (!request) return
        pending.delete(message.id)
        clearTimeout(request.timer)
        if (message.error)
            request.reject(new Error(JSON.stringify(message.error)))
        else request.resolve(message.result)
    }
    socket.onclose = () => {
        for (const request of pending.values()) {
            clearTimeout(request.timer)
            request.reject(new Error('Debugger connection closed'))
        }
        pending.clear()
    }
    const call = (method, params = {}) =>
        new Promise((resolve, reject) => {
            const id = ++nextId
            const timer = setTimeout(() => {
                pending.delete(id)
                reject(new Error('Debugger timeout: ' + method))
            }, 6000)
            pending.set(id, { resolve, reject, timer })
            socket.send(JSON.stringify({ id, method, params }))
        })
    return {
        call,
        async evaluate(expression) {
            const response = await call('Runtime.evaluate', {
                expression,
                returnByValue: true,
                awaitPromise: true,
            })
            if (response.exceptionDetails)
                throw new Error(JSON.stringify(response.exceptionDetails))
            return response.result.value
        },
        close: () => socket.close(),
    }
}

const rendererPort = await freePort()
const mainPort = await freePort()
const child = spawn(
    join(root, 'scripts/run-agent.sh'),
    [
        `--remote-debugging-port=${rendererPort}`,
        `--inspect=127.0.0.1:${mainPort}`,
        `--user-data-dir=${join(temporary, 'chromium')}`,
    ],
    {
        cwd: root,
        env: {
            ...process.env,
            XDG_CONFIG_HOME: join(temporary, 'config'),
            BACKUP_AGENT_STATE_DIR: join(temporary, 'agent-state'),
        },
        detached: true,
        stdio: ['ignore', 'pipe', 'pipe'],
    },
)
let output = ''
for (const stream of [child.stdout, child.stderr]) {
    stream.on('data', (data) => {
        output = (output + data).slice(-12000)
    })
}
let page
let main
try {
    page = await connect(rendererPort)
    main = await connect(mainPort)
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.window-control').length === 3",
            ),
        'React title bar',
    )
    await main.evaluate(`
    globalThis.testElectron = process.mainModule.require('electron');
    globalThis.testWindow = testElectron.BrowserWindow.getAllWindows()[0];
    globalThis.windowEvents = [];
    for (const name of ['minimize', 'restore', 'show', 'hide', 'focus', 'blur', 'maximize', 'unmaximize']) {
      testWindow.on(name, () => windowEvents.push({ name, time: Date.now(), minimized: testWindow.isMinimized(), visible: testWindow.isVisible() }));
    }
    globalThis.folderRequests = 0;
    // Test the full button -> preload -> IPC path without leaving a modal OS picker open.
    testElectron.dialog.showOpenDialog = async () => {
      folderRequests++;
      return { canceled: true, filePaths: [] };
    };
    testWindow.restore();
    if (testWindow.isMaximized()) testWindow.unmaximize();
  `)
    await until(
        () => main.evaluate('!testWindow.isMaximized()'),
        'initial restore',
    )
    await page.evaluate(`
    globalThis.windowTestClicks = [];
    document.addEventListener('click', event => {
      const button = event.target.closest('button');
      if (button) windowTestClicks.push({
        label: button.getAttribute('aria-label') || button.className,
        trusted: event.isTrusted,
      });
    }, true);
  `)

    async function click(selector) {
        const point = await page.evaluate(`(() => {
      const element = document.querySelector(${JSON.stringify(selector)});
      element.scrollIntoView({ block: 'center' });
      const r = element.getBoundingClientRect();
      return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
    })()`)
        await page.call('Input.dispatchMouseEvent', {
            type: 'mouseMoved',
            ...point,
        })
        await page.call('Input.dispatchMouseEvent', {
            type: 'mousePressed',
            button: 'left',
            buttons: 1,
            clickCount: 1,
            ...point,
        })
        await page.call('Input.dispatchMouseEvent', {
            type: 'mouseReleased',
            button: 'left',
            buttons: 0,
            clickCount: 1,
            ...point,
        })
    }

    async function refresh() {
        await until(
            () =>
                page.evaluate(
                    "!document.querySelector('.text-button').disabled",
                ),
            'connection button ready',
        )
        const before = await page.evaluate('windowTestClicks.length')
        await click('.text-button')
        await until(async () => {
            const events = await page.evaluate('windowTestClicks')
            return (
                events.length > before &&
                events.at(-1).label === 'text-button' &&
                events.at(-1).trusted
            )
        }, 'refresh mouse input')
        await until(
            () =>
                page.evaluate(
                    "!document.querySelector('.status-dot').classList.contains('checking')",
                ),
            'server response',
        )
    }

    if (nativeInput) {
        await page.evaluate(`document.title = ${JSON.stringify(testTitle)}`)
        await main.evaluate(`testWindow.setTitle(${JSON.stringify(testTitle)})`)
        async function nativeClick(selector) {
            const point = await page.evaluate(`(() => {
        const r = document.querySelector(${JSON.stringify(selector)}).getBoundingClientRect();
        return { x: (r.x + r.width / 2) / innerWidth, y: (r.y + r.height / 2) / innerHeight };
      })()`)
            await windowsInput('click', point)
        }
        for (let cycle = 0; cycle < 6; cycle++) {
            if (cycle % 2) {
                await nativeClick('.window-control:nth-child(2)')
                await until(
                    () => main.evaluate('testWindow.isMaximized()'),
                    'maximize before minimize',
                )
            }
            await nativeClick('.window-control:first-child')
            await until(
                async () => (await windowsInput('state')).minimized,
                'Windows minimize',
            )
            await until(
                () => main.evaluate('testWindow.isMinimized()'),
                'Electron minimize',
            )
            await windowsInput('restore')
            await until(
                async () => !(await windowsInput('state')).minimized,
                'Windows restore',
            )
            await until(
                () =>
                    main.evaluate(
                        '!testWindow.isMinimized() && testWindow.isVisible()',
                    ),
                'Electron restore',
            )
            await until(
                () => page.evaluate("document.visibilityState === 'visible'"),
                'Windows restore visibility',
            )
            await until(
                () =>
                    page.evaluate(
                        "!document.querySelector('.text-button').disabled",
                    ),
                'native refresh ready',
            )
            const before = await page.evaluate('windowTestClicks.length')
            await nativeClick('.text-button')
            await until(
                () => page.evaluate(`windowTestClicks.length > ${before}`),
                'Windows mouse input after restore',
            )
            if (await main.evaluate('testWindow.isMaximized()')) {
                await nativeClick('.window-control:nth-child(2)')
                await until(
                    () => main.evaluate('!testWindow.isMaximized()'),
                    'restore maximized window',
                )
            }
            await nativeClick('.window-control:nth-child(2)')
            await until(
                () => main.evaluate('testWindow.isMaximized()'),
                'Windows maximize after restore',
            )
            await nativeClick('.window-control:nth-child(2)')
            await until(
                () => main.evaluate('!testWindow.isMaximized()'),
                'Windows unmaximize',
            )
        }
        console.log(
            'PASS: 6 native Windows/WSLg minimize/restore/maximize cycles from normal and maximized windows',
        )
    }

    for (let cycle = 0; cycle < 5; cycle++) {
        await click('.window-control:nth-child(2)')
        await until(() => main.evaluate('testWindow.isMaximized()'), 'maximize')
        await until(
            () =>
                page.evaluate(
                    "document.querySelector('.window-control:nth-child(2)').ariaLabel === '还原'",
                ),
            'maximized icon',
        )
        await refresh()
        const before = await main.evaluate('folderRequests')
        await click('.primary-button')
        await until(
            () => main.evaluate(`folderRequests === ${before + 1}`),
            'directory chooser IPC',
        )
        await click('.window-control:nth-child(2)')
        await until(() => main.evaluate('!testWindow.isMaximized()'), 'restore')
        await until(
            () =>
                page.evaluate(
                    "document.querySelector('.window-control:nth-child(2)').ariaLabel === '最大化'",
                ),
            'restored icon',
        )
        await refresh()
    }
    console.log(
        'PASS: 5 maximize/restore cycles; refresh and directory entry remained responsive',
    )
    await until(
        () => /LISTENING 127\.0\.0\.1:\d+/.test(serverOutput),
        'backup server',
    )
    const backupPort = Number(
        serverOutput.match(/LISTENING 127\.0\.0\.1:(\d+)/)[1],
    )
    await page.evaluate(`window.backup.saveTarget({
    id: 'local', name: 'GUI test repository', host: '127.0.0.1', port: ${backupPort}
  })`)
    await refresh()
    await main.evaluate(`testElectron.dialog.showOpenDialog = async () => ({
    canceled: false, filePaths: [${JSON.stringify(sourceDirectory)}]
  })`)
    await click('.primary-button')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.task-card').length === 1",
            ),
        'persisted directory task',
    )
    await page.evaluate(
        `window.backup.saveTarget({ id: 'local', name: 'GUI test repository', host: '127.0.0.1', port: ${slowServer.address().port} })`,
    )
    await click('.text-button')
    await until(() => slowConnections.size > 0, 'stalled health check')
    assert(
        await page.evaluate(
            "!document.querySelector('.scan-button').disabled && !document.querySelector('.primary-button').disabled",
        ),
        'health checks must not disable local actions',
    )
    // Hold scan polling at RUNNING to exercise navigation deterministically.
    // The real Agent still scans; releasing the hold returns its actual result.
    await main.evaluate(`
    globalThis.scanAgentPrototype = process.mainModule.require(
        testElectron.app.getAppPath() + '/dist-electron/agent-bridge.js'
    ).AgentBridge.prototype;
    globalThis.originalAgentSend = scanAgentPrototype.send;
    globalThis.holdScanStatus = true;
    globalThis.heldScanId = null;
    globalThis.scanStatusPolls = 0;
    scanAgentPrototype.send = async function(action, ...args) {
        const result = await originalAgentSend.call(this, action, ...args);
        if (action === 'start_scan' && holdScanStatus) {
            heldScanId = result.operation_id;
        }
        if (action === 'operation' && result.id === heldScanId) {
            scanStatusPolls++;
            if (holdScanStatus) {
                return {
                    ...result, state: 'RUNNING', stage: 'scan',
                    result: undefined, finished_at: undefined
                };
            }
        }
        return result;
    };
  `)
    await click('.scan-button')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.task-card .task-scan-status')?.textContent.includes('扫描')",
            ),
        'queued scan feedback',
    )
    for (const tab of ['versions', 'targets']) {
        await click(`[data-tab="${tab}"]`)
        assert.equal(
            await page.evaluate(
                "document.querySelector('.nav-item.active').dataset.tab",
            ),
            tab,
        )
        assert(
            await page.evaluate(
                "document.querySelector('.operation-panel, .pending-operation') === null",
            ),
            `queued scan must not appear on ${tab}`,
        )
    }
    await click('[data-tab="records"]')
    assert.equal(
        await page.evaluate("document.querySelector('h1').textContent"),
        '执行记录',
    )
    assert(
        await page.evaluate(
            "document.querySelector('.operation-panel, .pending-operation') === null",
        ),
        'queued scan must not open details in records',
    )
    for (const socket of slowConnections) socket.destroy()
    await until(
        () =>
            page.evaluate(
                "document.querySelector('tbody tr .state-label')?.textContent === '执行中'",
            ),
        'scan record updates after failed check',
    )
    assert(
        await page.evaluate(
            "document.querySelector('.operation-panel, .pending-operation') === null",
        ),
        'running scan must not automatically open details in records',
    )
    await click('.record-view')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.operation-panel h2')?.textContent === '扫描 · 执行中'",
            ),
        'explicitly view running scan details',
    )
    await click('[aria-label="收起执行结果"]')
    const pollsBeforeClose = await main.evaluate('scanStatusPolls')
    await until(
        () => main.evaluate(`scanStatusPolls > ${pollsBeforeClose + 1}`),
        'scan continues after closing record details',
    )
    assert(
        await page.evaluate(
            "document.querySelector('.operation-panel') === null",
        ),
        'polling must not reopen a closed record',
    )
    await click('.record-view')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.operation-panel') !== null",
            ),
        'reopen running record before navigation',
    )
    for (const tab of ['tasks', 'versions', 'targets', 'records']) {
        await click(`[data-tab="${tab}"]`)
        assert.equal(
            await page.evaluate(
                "document.querySelector('.nav-item.active').dataset.tab",
            ),
            tab,
        )
        const polls = await main.evaluate('scanStatusPolls')
        await until(
            () => main.evaluate(`scanStatusPolls > ${polls}`),
            `scan polling continues on ${tab}`,
        )
        assert(
            await page.evaluate(
                "document.querySelector('.operation-panel, .pending-operation') === null",
            ),
            `running scan must not appear above ${tab}`,
        )
        if (tab === 'tasks') {
            assert(
                await page.evaluate(
                    "document.querySelector('.task-card .task-scan-progress h3')?.textContent === '扫描 · 执行中'",
                ),
                'running scan remains visible in its task card',
            )
        }
    }
    const recordsScreenshot = await page.call('Page.captureScreenshot', {
        format: 'png',
    })
    await writeFile(
        join(tmpdir(), 'backup-records-during-scan.png'),
        Buffer.from(recordsScreenshot.data, 'base64'),
    )
    await click('[data-tab="versions"]')
    assert.equal(
        await page.evaluate("document.querySelector('h1').textContent"),
        '备份版本',
    )
    await main.evaluate('holdScanStatus = false')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.target-selector select').disabled === false",
            ),
        'scan finishes while viewing versions',
    )
    assert(
        await page.evaluate(
            "document.querySelector('.operation-panel, .pending-operation') === null",
        ),
        'scan completion must not appear on versions',
    )
    const firstScanId = await main.evaluate('heldScanId')
    await until(() => slowConnections.size > 0, 'post-scan health check')
    for (const socket of slowConnections) socket.destroy()
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.status-dot')?.classList.contains('offline')",
            ),
        'offline state',
    )
    await page.evaluate(
        `window.backup.saveTarget({ id: 'local', name: 'GUI test repository', host: '127.0.0.1', port: ${backupPort} })`,
    )
    await click('[data-tab="tasks"]')
    await refresh()
    assert(
        await page.evaluate(
            "document.querySelector('.task-card .scan-section') !== null",
        ),
        'returning to tasks retains the completed scan preview',
    )
    await click('[data-tab="records"]')
    await until(
        () => page.evaluate("document.querySelector('.record-view') !== null"),
        'completed scan record',
    )
    assert(
        await page.evaluate(
            "document.querySelector('.operation-panel') === null",
        ),
        'completed scan details also require an explicit selection',
    )
    await click('.record-view')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.operation-panel h2')?.textContent === '扫描 · 成功'",
            ),
        'scan result remains accessible from records',
    )
    console.log(
        'PASS: scan progress stays in its task; record details open only on request and stay closed during polling',
    )
    await click('[data-tab="tasks"]')
    await main.evaluate('holdScanStatus = true')
    await click('.task-card .scan-button')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.task-card .task-scan-progress h3')?.textContent === '扫描 · 执行中'",
            ),
        'second scan running',
    )
    await click('[data-tab="records"]')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.scan-history-count')?.textContent.includes('共 2 次')",
            ),
        'same-task scans grouped with a total count',
    )
    assert.equal(
        await page.evaluate(
            "document.querySelectorAll('.records-table tbody tr').length",
        ),
        1,
    )
    await click('.scan-group-toggle')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.scan-history-row').length === 2",
            ),
        'expanded scan history',
    )
    await click(
        `.scan-history-row[data-operation-id="${firstScanId}"] .record-view`,
    )
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.operation-panel h2')?.textContent === '扫描 · 成功'",
            ),
        'previous scan details during a new scan',
    )
    const pollsBeforeHistory = await main.evaluate('scanStatusPolls')
    await until(
        () => main.evaluate(`scanStatusPolls > ${pollsBeforeHistory + 1}`),
        'poll current scan while reading history',
    )
    assert(
        await page.evaluate(
            `document.querySelector('.operation-details')?.textContent.includes(${JSON.stringify(firstScanId)}) && document.querySelector('.operation-panel h2')?.textContent === '扫描 · 成功'`,
        ),
        'current scan must not replace the selected historical record',
    )
    await click('[aria-label="收起执行结果"]')
    await main.evaluate('holdScanStatus = false')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.target-selector select').disabled === false && document.querySelector('tbody tr .state-label')?.textContent === '成功'",
            ),
        'scan completes while viewing records',
    )
    assert(
        await page.evaluate(
            "document.querySelector('.operation-panel, .pending-operation') === null",
        ),
        'completion must not open record details',
    )
    await main.evaluate('scanAgentPrototype.send = originalAgentSend')
    console.log(
        'PASS: selected history survives active scan updates; completion updates the record row without opening details',
    )
    await click('[data-tab="tasks"]')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.scan-section') && !document.querySelector('.scan-backup').disabled",
            ),
        'scan preview',
    )
    assert(
        await page.evaluate(
            "document.querySelector('.digest-short').scrollWidth > document.querySelector('.digest-short').clientWidth",
        ),
    )
    await click('.digest-details summary')
    assert.equal(
        (
            await page.evaluate(
                "document.querySelector('.digest-details code').textContent",
            )
        ).length,
        64,
    )
    const firstTask = (await page.evaluate('window.backup.getConfig()'))
        .tasks[0]
    const firstCard = `.task-card[data-task-id="${firstTask.id}"]`
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [${JSON.stringify(secondSourceDirectory)}] })`,
    )
    await click('.primary-button')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.task-card').length === 2",
            ),
        'second task',
    )
    const secondTask = (
        await page.evaluate('window.backup.getConfig()')
    ).tasks.find((task) => task.id !== firstTask.id)
    const secondCard = `.task-card[data-task-id="${secondTask.id}"]`
    await click(`${secondCard} .scan-button`)
    await until(
        () =>
            page.evaluate(
                `document.querySelector(${JSON.stringify(`${secondCard} .scan-backup`)})?.disabled === false`,
            ),
        'second preview',
    )
    const previews =
        await page.evaluate(`Array.from(document.querySelectorAll('.task-card')).map(card => ({
        id: card.dataset.taskId,
        source: card.querySelector('.task-copy .path').textContent,
        file: card.querySelector('.scan-table tr[data-type="file"] td').textContent,
        warnings: card.querySelector('.scan-warnings summary')?.textContent || '',
    }))`)
    assert.deepEqual(
        previews.map((item) => [item.id, item.source, item.file]),
        [
            [firstTask.id, sourceDirectory, '中文 file.txt'],
            [secondTask.id, secondSourceDirectory, 'second.txt'],
        ],
    )
    assert.equal(previews[0].warnings, '')
    assert.equal(previews[1].warnings, '201 项警告')
    assert.equal(
        await page.evaluate(
            "document.querySelectorAll('.warning-list p').length",
        ),
        0,
    )
    await click(`${secondCard} .scan-warnings summary`)
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.warning-list p').length === 100",
            ),
        'first warning page loaded on demand',
    )
    await click('.warning-pagination button')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.warning-list p').length === 200",
            ),
        'second warning page',
    )
    await click('.warning-pagination button')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.warning-list p').length === 201 && !document.querySelector('.warning-pagination button')",
            ),
        'all warnings accessible without duplication',
    )
    await click(`${secondCard} .scan-warnings summary`)
    assert(
        await page.evaluate(
            "document.querySelector('.operation-panel') === null",
        ),
        'scan status belongs to its task',
    )
    await click(`${firstCard} .preview-toggle`)
    assert(
        await page.evaluate(
            `!document.querySelector(${JSON.stringify(`${firstCard} .scan-table`)}) && !!document.querySelector(${JSON.stringify(`${secondCard} .scan-table`)})`,
        ),
        'collapse affects only the selected task',
    )
    for (const [width, height, filename] of [
        [1120, 760, 'backup-scan-cards.png'],
        [850, 600, 'backup-scan-cards-narrow.png'],
    ]) {
        await main.evaluate(`testWindow.setSize(${width}, ${height})`)
        await delay(150)
        await page.evaluate("document.querySelector('.content').scrollTop = 0")
        assert(
            await page.evaluate(
                "document.documentElement.scrollWidth <= innerWidth && [...document.querySelectorAll('.task-card')].every(card => card.scrollWidth <= card.clientWidth)",
            ),
            'task preview fits its card',
        )
        const capture = await page.call('Page.captureScreenshot', {
            format: 'png',
        })
        await writeFile(
            join(tmpdir(), filename),
            Buffer.from(capture.data, 'base64'),
        )
    }
    await main.evaluate('testWindow.setSize(1120, 760)')
    await click(`${secondCard} [aria-label="移除任务"]`)
    await click('.danger-button')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.task-card').length === 1",
            ),
        'remove only second task',
    )
    assert(
        await page.evaluate(
            `!!document.querySelector(${JSON.stringify(`${firstCard} .scan-section`)})`,
        ),
        'first preview survives removing another task',
    )
    console.log(
        'PASS: two same-name tasks retain distinct previews, warnings, collapse state and 850px layout',
    )

    async function holdOperation(action) {
        await main.evaluate(`
        globalThis.heldAction = ${JSON.stringify(action)};
        globalThis.heldOperationId = null;
        globalThis.operationPolls = 0;
        globalThis.holdOperationStatus = true;
        globalThis.startGate = new Promise(resolve => {
            globalThis.releaseOperationStart = resolve;
        });
        scanAgentPrototype.send = async function(command, ...args) {
            if (command === 'start_' + heldAction) await startGate;
            const result = await originalAgentSend.call(this, command, ...args);
            if (command === 'start_' + heldAction) {
                heldOperationId = result.operation_id;
            }
            if (command === 'operation' && result.id === heldOperationId) {
                operationPolls++;
                if (holdOperationStatus) {
                    return {
                        ...result, state: 'RUNNING', stage: heldAction,
                        result: undefined, finished_at: undefined
                    };
                }
            }
            return result;
        };
      `)
    }

    async function checkOtherPages(tabs, polling) {
        for (const tab of tabs) {
            await click(`[data-tab="${tab}"]`)
            if (polling) {
                const polls = await main.evaluate('operationPolls')
                await until(
                    () => main.evaluate(`operationPolls > ${polls}`),
                    `operation continues on ${tab}`,
                )
            }
            assert(
                await page.evaluate(
                    "document.querySelector('.content > .operation-panel, .pending-operation, .task-backup-status') === null",
                ),
                `operation must not open unrelated progress on ${tab}`,
            )
        }
    }

    await holdOperation('backup')
    await click('.scan-backup')
    await until(
        () =>
            page.evaluate(
                `document.querySelector(${JSON.stringify(`${firstCard} .task-backup-status`)})?.textContent.includes('正在准备备份')`,
            ),
        'backup preparation in the owning task',
    )
    await checkOtherPages(['versions', 'targets', 'records'], false)
    await main.evaluate('releaseOperationStart()')
    await until(
        () => main.evaluate('operationPolls > 1'),
        'backup polling while reading records',
    )
    await checkOtherPages(['versions', 'targets', 'records'], true)
    const backupId = await main.evaluate('heldOperationId')
    await click(`tr[data-operation-id="${backupId}"] .record-view`)
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.operation-panel h2')?.textContent === '备份 · 执行中'",
            ),
        'explicit backup progress in records',
    )
    await click('[aria-label="收起执行结果"]')
    await checkOtherPages(['records'], true)
    await click('[data-tab="tasks"]')
    await until(
        () =>
            page.evaluate(
                `document.querySelector(${JSON.stringify(`${firstCard} .operation-panel h3`)})?.textContent === '备份 · 执行中'`,
            ),
        'backup progress remains in the owning task',
    )
    assert(
        await page.evaluate(
            "document.querySelector('.content > .operation-panel') === null",
        ),
        'backup must not create a global progress panel',
    )
    // The real version is committed. Model an uncertain commit receipt so
    // the task-bound confirmation button must resolve the persisted record.
    const backupRecordPath = join(
        temporary,
        'agent-state',
        'operations',
        `${backupId}.json`,
    )
    await until(async () => {
        const record = JSON.parse(await readFile(backupRecordPath, 'utf8'))
        return record.state === 'SUCCEEDED'
    }, 'real backup committed')
    const pendingBackup = JSON.parse(await readFile(backupRecordPath, 'utf8'))
    pendingBackup.state = 'WAITING'
    pendingBackup.error = 'Commit receipt pending confirmation'
    await writeFile(backupRecordPath, JSON.stringify(pendingBackup))
    await click('[data-tab="targets"]')
    await main.evaluate('holdOperationStatus = false')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.target-selector select').disabled === false",
            ),
        'backup leaves running state on another page',
    )
    await checkOtherPages(['versions', 'targets', 'records'], false)
    await click('[data-tab="tasks"]')
    await until(
        () =>
            page.evaluate(
                `document.querySelector(${JSON.stringify(`${firstCard} .operation-panel h3`)})?.textContent.includes('待确认')`,
            ),
        'pending commit retained on returning to the task',
    )
    await click(`${firstCard} .result-actions button`)
    await until(
        () =>
            page.evaluate(
                `document.querySelector(${JSON.stringify(`${firstCard} .operation-panel h3`)})?.textContent === '备份 · 成功'`,
            ),
        'task-bound backup confirmation',
        15000,
    )
    assert(
        await page.evaluate(
            `document.querySelector(${JSON.stringify(`${firstCard} .scan-section`)}) === null`,
        ),
        'confirmed backup clears only its prior scan preview',
    )
    await main.evaluate('scanAgentPrototype.send = originalAgentSend')
    const operation = await page.evaluate('window.backup.listOperations()')
    assert.equal(operation.operations[0].state, 'SUCCEEDED')
    assert.equal(operation.operations[0].task_id, firstTask.id)
    for (const [width, height, filename] of [
        [1120, 760, 'backup-desktop-tasks.png'],
        [850, 600, 'backup-task-result-narrow.png'],
    ]) {
        await main.evaluate(`testWindow.setSize(${width}, ${height})`)
        await delay(150)
        await page.evaluate("document.querySelector('.content').scrollTop = 0")
        assert(
            await page.evaluate(
                "document.documentElement.scrollWidth <= innerWidth && [...document.querySelectorAll('.task-card')].every(card => card.scrollWidth <= card.clientWidth)",
            ),
            'task backup result fits both supported window sizes',
        )
        const screenshot = await page.call('Page.captureScreenshot', {
            format: 'png',
        })
        await writeFile(
            join(tmpdir(), filename),
            Buffer.from(screenshot.data, 'base64'),
        )
    }
    await main.evaluate('testWindow.setSize(1120, 760)')
    console.log(
        'PASS: backup preparation/progress/result stay in the owning task; records require selection; pending commit confirmation works',
    )
    await click('.view-versions')
    await until(
        () =>
            page.evaluate("document.querySelectorAll('tbody tr').length === 1"),
        'committed version table',
    )
    assert.equal(
        await page.evaluate(
            "document.querySelector('.operation-panel') === null",
        ),
        true,
    )
    assert(
        await page.evaluate(
            "document.querySelector('.version-filter select').value !== ''",
        ),
    )
    await main.evaluate(`testElectron.dialog.showOpenDialog = async () => ({
    canceled: false, filePaths: [${JSON.stringify(restoreDirectory)}]
  })`)
    await click('tbody tr button')
    await until(
        () => page.evaluate("document.querySelector('dialog[open]') !== null"),
        'restore confirmation',
    )
    assert(
        await page.evaluate(
            "document.querySelector('.confirm-restore').disabled",
        ),
    )
    await click('[aria-label="选择还原目录"]')
    await until(
        () =>
            page.evaluate(
                "!document.querySelector('.confirm-restore').disabled",
            ),
        'selected restore directory',
    )
    assert.deepEqual(await readdir(restoreDirectory), [])
    const restoreScreenshot = await page.call('Page.captureScreenshot', {
        format: 'png',
    })
    await writeFile(
        join(tmpdir(), 'backup-desktop-restore.png'),
        Buffer.from(restoreScreenshot.data, 'base64'),
    )
    assert(
        await page.evaluate(
            "[...document.querySelectorAll('.table-scroll')].every(element => element.scrollWidth <= element.clientWidth)",
        ),
        'version table should fit its container',
    )
    await holdOperation('restore')
    await click('.confirm-restore')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.pending-operation')?.textContent.includes('正在准备还原')",
            ),
        'restore preparation on versions',
    )
    await checkOtherPages(['tasks', 'targets', 'records'], false)
    await main.evaluate('releaseOperationStart()')
    await until(
        () => main.evaluate('operationPolls > 1'),
        'restore polling while reading records',
    )
    await checkOtherPages(['tasks', 'targets', 'records'], true)
    await click('[data-tab="versions"]')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.operation-panel h2')?.textContent === '还原 · 执行中'",
            ),
        'restore progress on versions',
    )
    await click('[data-tab="targets"]')
    await main.evaluate('holdOperationStatus = false')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.target-selector select').disabled === false",
            ),
        'restore completes on another page',
    )
    await checkOtherPages(['targets', 'records'], false)
    await click('[data-tab="versions"]')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.operation-panel h2')?.textContent === '还原 · 成功'",
            ),
        'real restore completion',
        15000,
    )
    assert.equal(
        await readFile(join(restoreDirectory, '中文 file.txt'), 'utf8'),
        'desktop backup roundtrip\n',
    )
    await click('.restore-details summary')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.restore-details')?.textContent.includes('已写入 2 项')",
            ),
        'persisted restore entries in the desktop',
    )
    await main.evaluate('scanAgentPrototype.send = originalAgentSend')
    await click('[data-tab="tasks"]')
    assert(
        await page.evaluate(
            `document.querySelector(${JSON.stringify(`${firstCard} .operation-panel h3`)})?.textContent === '备份 · 成功'`,
        ),
        'restoring must not replace the task backup result',
    )
    await click(`${firstCard} [aria-label="收起执行结果"]`)
    await click('[data-tab="versions"]')
    await click('[data-tab="tasks"]')
    assert(
        await page.evaluate(
            "document.querySelector('.operation-panel') === null",
        ),
        'closing the task result persists across navigation',
    )
    console.log(
        'PASS: restore preparation/progress/result stay on versions; task backup results survive navigation and can be dismissed',
    )
    await click('.nav-item:nth-of-type(3)')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.records-table tbody tr').length === 4 && [...document.querySelectorAll('.scan-history-count')].some(item => item.textContent.includes('共 2 次'))",
            ),
        'persistent execution records with grouped scans',
    )
    for (let index = 0; index < 103; index++) {
        const record = {
            id: `history-${index}`,
            action: 'scan',
            stage: 'scan',
            task_id: firstTask.id,
            source: firstTask.path,
            target: { id: 'local' },
            files: 1,
            bytes: 25,
            warning_count: 0,
            state: index === 0 ? 'FAILED' : 'SUCCEEDED',
            started_at: new Date(Date.UTC(2020, 0, 1, 0, index)).toISOString(),
        }
        await writeFile(
            join(temporary, 'agent-state', 'operations', `${record.id}.json`),
            JSON.stringify(record),
        )
    }
    await click('.records-refresh')
    await until(
        () =>
            page.evaluate(
                "[...document.querySelectorAll('.scan-history-count')].some(item => item.textContent.includes('共 105 次') && item.textContent.includes('1 次异常'))",
            ),
        'scan group includes all history and failures',
    )
    const groupedScanId = await page.evaluate(
        "[...document.querySelectorAll('.scan-history-count')].find(item => item.textContent.includes('共 105 次')).closest('tr').dataset.operationId",
    )
    const groupToggle = `.scan-group[data-operation-id="${groupedScanId}"] .scan-group-toggle`
    await click(groupToggle)
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.scan-history-row').length === 100",
            ),
        'first scan-history page',
    )
    await click('.scan-history-pagination button')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.scan-history-row').length === 105 && !document.querySelector('.scan-history-pagination')",
            ),
        'remaining scan-history page',
    )
    assert.equal(
        await page.evaluate(
            "new Set([...document.querySelectorAll('.scan-history-row')].map(row => row.dataset.operationId)).size",
        ),
        105,
    )
    await click(groupToggle)
    for (const [width, height, filename] of [
        [1120, 760, 'backup-records-grouped.png'],
        [850, 600, 'backup-records-grouped-narrow.png'],
    ]) {
        await main.evaluate(`testWindow.setSize(${width}, ${height})`)
        await delay(150)
        assert(
            await page.evaluate(
                "document.documentElement.scrollWidth <= innerWidth && document.querySelector('.records-table').scrollWidth <= document.querySelector('.table-scroll').clientWidth",
            ),
            'grouped records fit the window',
        )
        const capture = await page.call('Page.captureScreenshot', {
            format: 'png',
        })
        await writeFile(
            join(tmpdir(), filename),
            Buffer.from(capture.data, 'base64'),
        )
    }
    console.log(
        'PASS: scan histories are collapsed by task; warning details load in 100-item pages; grouped records fit both window sizes',
    )
    await page.call('Page.reload')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.task-card').length === 1",
            ),
        'reload retains tasks',
    )
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.status-dot')?.classList.contains('online')",
            ),
        'service after reload',
    )
    await checkP0({
        page,
        main,
        click,
        refresh,
        until,
        temporary,
        sourceDirectory,
        backupId,
    })
    await main.evaluate('testWindow.setSize(850, 600)')
    await delay(150)
    const narrow = await page.call('Page.captureScreenshot', { format: 'png' })
    await writeFile(
        join(tmpdir(), 'backup-desktop-narrow.png'),
        Buffer.from(narrow.data, 'base64'),
    )
    assert(
        await page.evaluate(
            'document.documentElement.scrollWidth <= window.innerWidth',
        ),
    )
    await click('[aria-label="移除任务"]')
    await until(
        () => page.evaluate("document.querySelector('dialog[open]') !== null"),
        'remove confirmation',
    )
    await page.call('Input.dispatchKeyEvent', {
        type: 'keyDown',
        key: 'Escape',
        code: 'Escape',
        windowsVirtualKeyCode: 27,
    })
    await page.call('Input.dispatchKeyEvent', {
        type: 'keyUp',
        key: 'Escape',
        code: 'Escape',
        windowsVirtualKeyCode: 27,
    })
    await until(
        () => page.evaluate("document.querySelector('dialog[open]') === null"),
        'cancel remove',
    )
    assert.equal(
        await page.evaluate("document.querySelectorAll('.task-card').length"),
        1,
    )
    await click('[aria-label="移除任务"]')
    await click('.danger-button')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.task-card').length === 0",
            ),
        'confirmed task removal',
    )
    assert.equal(
        await readFile(join(sourceDirectory, '中文 file.txt'), 'utf8'),
        'desktop backup roundtrip\n',
    )
    await click('[data-tab="versions"]')
    await until(
        () =>
            page.evaluate("document.querySelectorAll('tbody tr').length === 1"),
        'versions survive task removal',
    )
    console.log(
        'PASS: stalled connection preserves navigation/local scanning; confirmed removal preserves source and versions',
    )
    console.log(
        'PASS: real task/backup/version/restore/records/reload and 850px layout; desktop screenshots in /tmp',
    )
    await page.evaluate(`
    globalThis.windowTestClicks = [];
    document.addEventListener('click', event => {
      const button = event.target.closest('button');
      if (button) windowTestClicks.push({ label: button.getAttribute('aria-label') || button.className, trusted: event.isTrusted });
    }, true);
  `)
    await click('.window-control:first-child')
    await until(() => main.evaluate('testWindow.isMinimized()'), 'minimize')
    await page.evaluate(
        'globalThis.backgroundTick = 0; globalThis.backgroundTimer = setInterval(() => backgroundTick++, 100)',
    )
    await delay(500)
    assert(
        await page.evaluate('backgroundTick >= 2'),
        'background progress timers should continue',
    )
    await page.evaluate('clearInterval(backgroundTimer)')
    await main.evaluate('testWindow.restore(); testWindow.show()')
    await until(
        () => page.evaluate("document.visibilityState === 'visible'"),
        'restore visibility',
    )
    await refresh()
    console.log('PASS: minimize, restore and refresh')
    try {
        await click('.window-control.close')
    } catch (error) {
        if (!/connection closed|Cannot find context/.test(String(error)))
            throw error
    }
    main.close()
    page.close()
    await until(() => child.exitCode !== null, 'application exit')
    assert.equal(child.exitCode, 0)
    assert(
        !output.includes('drmGetDevices2() has not found any devices'),
        'unexpected DRM device probe on software rendering path',
    )
    console.log(
        'PASS: workflow confirmations, window recovery, background timers, close and no missing-DRM errors',
    )
} catch (error) {
    try {
        console.error(
            'WINDOW STATE',
            await main.evaluate(
                '({ events: windowEvents, minimized: testWindow.isMinimized(), visible: testWindow.isVisible(), focused: testWindow.isFocused() })',
            ),
        )
        console.error(
            'PAGE STATE',
            await page.evaluate(
                '({ visibility: document.visibilityState, focused: document.hasFocus() })',
            ),
        )
        if (nativeInput)
            console.error('WINDOWS STATE', await windowsInput('state'))
    } catch {}
    console.error(output)
    throw error
} finally {
    page?.close()
    main?.close()
    if (child.exitCode === null) {
        try {
            process.kill(-child.pid, 'SIGTERM')
        } catch {}
        await delay(300)
    }
    if (server.exitCode === null && server.signalCode === null) {
        server.kill('SIGTERM')
        await until(
            () => server.exitCode !== null || server.signalCode !== null,
            'backup server shutdown',
        )
    }
    for (const socket of slowConnections) socket.destroy()
    await new Promise((resolve) => slowServer.close(resolve))
    await rm(temporary, { recursive: true, force: true })
}
