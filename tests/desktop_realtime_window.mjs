import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { createServer } from 'node:net'
import {
    mkdir,
    mkdtemp,
    readFile,
    writeFile,
    rename,
    rm,
} from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { fileURLToPath } from 'node:url'
import { setTimeout as delay } from 'node:timers/promises'
import { requireGlass } from './desktop_glass.mjs'

const root = fileURLToPath(new URL('../', import.meta.url))
const temporary = await mkdtemp(join(tmpdir(), 'backup-realtime-window-'))
const children = []
let page, main
let output = ''
async function until(check, label, timeout = 15000) {
    const deadline = Date.now() + timeout
    while (Date.now() < deadline) {
        if (await check()) return
        await delay(80)
    }
    throw new Error('Timed out: ' + label)
}
async function port() {
    const server = createServer()
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve))
    const value = server.address().port
    await new Promise((resolve) => server.close(resolve))
    return value
}
async function connect(port) {
    let url
    await until(
        async () => {
            try {
                url = (
                    await (
                        await fetch(`http://127.0.0.1:${port}/json/list`)
                    ).json()
                )[0]?.webSocketDebuggerUrl
                return !!url
            } catch {
                return false
            }
        },
        'debugger startup',
        30000,
    )
    const socket = new WebSocket(url)
    await new Promise((resolve, reject) => {
        socket.onopen = resolve
        socket.onerror = reject
    })
    const pending = new Map()
    let sequence = 0
    socket.onmessage = (event) => {
        const reply = JSON.parse(event.data)
        const request = pending.get(reply.id)
        if (!request) return
        pending.delete(reply.id)
        clearTimeout(request.timer)
        if (reply.error) request.reject(new Error(JSON.stringify(reply.error)))
        else request.resolve(reply.result)
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
            const id = ++sequence
            const timer = setTimeout(() => {
                pending.delete(id)
                reject(new Error('Debugger timeout: ' + method))
            }, 10000)
            pending.set(id, { resolve, reject, timer })
            socket.send(JSON.stringify({ id, method, params }))
        })
    return {
        call,
        async evaluate(expression) {
            const reply = await call('Runtime.evaluate', {
                expression,
                returnByValue: true,
                awaitPromise: true,
            })
            if (reply.exceptionDetails)
                throw new Error(JSON.stringify(reply.exceptionDetails))
            return reply.result.value
        },
        close: () => socket.close(),
    }
}
function launch(script, args, env) {
    const child = spawn(join(root, script), args, {
        cwd: root,
        env: { ...process.env, ...env },
        detached: true,
        stdio: ['ignore', 'pipe', 'pipe'],
    })
    children.push(child)
    for (const stream of [child.stdout, child.stderr])
        stream.on('data', (data) => {
            output = (output + data).slice(-18000)
        })
    return child
}
async function server(name) {
    const child = launch('scripts/run-server.sh', ['--port', '0'], {
        BACKUP_DATA_DIR: join(temporary, name),
    })
    let text = ''
    child.stdout.on('data', (data) => {
        text += data
    })
    await until(
        () => /LISTENING 127\.0\.0\.1:(\d+)/.test(text),
        'server startup',
    )
    return {
        child,
        port: Number(text.match(/LISTENING 127\.0\.0\.1:(\d+)/)[1]),
    }
}
async function click(selector) {
    let point
    await until(async () => {
        point = await page.evaluate(`(() => {
            const element = document.querySelector(${JSON.stringify(selector)});
            if (!element || element.disabled) return null;
            element.scrollIntoView({block:'center'});
            const r = element.getBoundingClientRect();
            return r.width && r.height ? {x:r.x+r.width/2,y:r.y+r.height/2} : null;
        })()`)
        return point !== null
    }, 'click ' + selector)
    for (const [type, buttons] of [
        ['mouseMoved', 0],
        ['mousePressed', 1],
        ['mouseReleased', 0],
    ]) {
        await page.call('Input.dispatchMouseEvent', {
            type,
            ...point,
            button: type === 'mouseMoved' ? 'none' : 'left',
            buttons,
            clickCount: 1,
        })
    }
}
async function finished(action, success = true) {
    await until(
        () =>
            page.evaluate(
                `document.querySelector('.operation-panel h2')?.textContent.includes('${action} · ${success ? '成功' : '失败'}')`,
            ),
        action,
        45000,
    )
}
async function operation(id) {
    let record
    await until(
        async () => {
            record = await page.evaluate(`window.backup.getOperation('${id}')`)
            return record.state !== 'RUNNING'
        },
        'operation completion',
        45000,
    )
    assert(record.state.startsWith('SUCCEEDED'), JSON.stringify(record))
    return record
}
let daemonPid
try {
    const backend = await server('repository')
    const state = join(temporary, 'agent-state')
    const source = join(temporary, 'source')
    await mkdir(state)
    await mkdir(source)
    await writeFile(join(source, 'example.txt'), 'before close')
    await writeFile(
        join(state, 'config.json'),
        JSON.stringify({
            format: 8,
            tasks: [
                {
                    id: 'realtime-window',
                    path: source,
                    target_id: 'local',
                    createdAt: new Date().toISOString(),
                },
            ],
            targets: [
                {
                    id: 'local',
                    name: '实时备份验证',
                    host: '127.0.0.1',
                    port: backend.port,
                },
            ],
        }),
    )
    async function openWindow() {
        const rendererPort = await port()
        const mainPort = await port()
        const child = launch(
            'scripts/run-agent.sh',
            [
                `--remote-debugging-port=${rendererPort}`,
                `--inspect=127.0.0.1:${mainPort}`,
                `--user-data-dir=${join(temporary, 'chromium')}`,
            ],
            {
                BACKUP_AGENT_STATE_DIR: state,
                XDG_CONFIG_HOME: join(temporary, 'config'),
            },
        )
        page = await connect(rendererPort)
        main = await connect(mainPort)
        await requireGlass(page, until)
        await main.evaluate(
            `globalThis.testElectron = process.getBuiltinModule('module').createRequire(process.cwd() + '/package.json')('electron'); globalThis.testWindow = testElectron.BrowserWindow.getAllWindows()[0]; testWindow.setSize(850,600); testWindow.show();`,
        )
        await until(
            () =>
                page.evaluate(
                    `!!document.querySelector('input[aria-label="实时备份"]')`,
                ),
            'realtime switch',
        )
        return child
    }
    let electron = await openWindow()
    daemonPid = (await page.evaluate('window.backup.getConfig()')).agent_pid
    await click('input[aria-label="实时备份"]')
    await until(
        () =>
            page.evaluate(
                `document.querySelector('.realtime-toggle')?.textContent.includes('监控中')`,
            ),
        'initial realtime backup',
        30000,
    )
    const first = await page.evaluate('window.backup.listOperations()')
    assert(
        first.operations.some(
            (item) => item.action === 'backup' && item.state === 'SUCCEEDED',
        ),
    )
    const screenshot = await page.call('Page.captureScreenshot', {
        format: 'png',
    })
    await writeFile(
        '/tmp/backup-realtime-850.png',
        Buffer.from(screenshot.data, 'base64'),
    )
    assert(
        await page.evaluate(
            'document.documentElement.scrollWidth <= innerWidth',
        ),
    )
    await main.evaluate('testWindow.close()')
    main.close()
    page.close()
    await until(() => electron.exitCode !== null, 'first window closes')
    assert.equal(electron.exitCode, 0)
    await writeFile(join(source, 'example.txt'), 'after GUI closed')
    await delay(4000)
    electron = await openWindow()
    const config = await page.evaluate('window.backup.getConfig()')
    assert.equal(config.agent_pid, daemonPid)
    assert.equal(config.tasks[0].realtime, true)
    const rows = (await page.evaluate('window.backup.listOperations()'))
        .operations
    assert(
        rows.filter(
            (item) => item.action === 'backup' && item.state === 'SUCCEEDED',
        ).length >= 2,
    )
    const version = rows.find(
        (item) => item.action === 'backup' && item.state === 'SUCCEEDED',
    )
    const started = await page.evaluate(
        `window.backup.restoreVersion('local', '${version.id}', ${JSON.stringify(join(temporary, 'restored'))})`,
    )
    await operation(started.operation_id)
    assert.equal(
        await readFile(join(temporary, 'restored/example.txt'), 'utf8'),
        'after GUI closed',
    )
    await click('input[aria-label="实时备份"]')
    await until(
        () =>
            page.evaluate(
                `document.querySelector('.realtime-toggle')?.textContent.includes('已关闭')`,
            ),
        'disable',
    )
    await main.evaluate('testWindow.close()')
    main.close()
    page.close()
    await until(() => electron.exitCode !== null, 'second window closes')
    assert.equal(electron.exitCode, 0)
    console.log(
        'PASS: actual checkbox, initial automatic backup, 850px layout, closed GUI backup, reopen same daemon/history, restore new data, disable',
    )
} catch (error) {
    console.error(output)
    try {
        console.error(
            await page?.evaluate('document.body.innerText.slice(-6000)'),
        )
    } catch {}
    throw error
} finally {
    page?.close()
    main?.close()
    if (daemonPid) {
        try {
            process.kill(daemonPid, 'SIGTERM')
        } catch {}
    }
    for (const child of children.reverse()) {
        if (child.exitCode === null) {
            try {
                process.kill(-child.pid, 'SIGTERM')
            } catch {}
        }
    }
    await delay(300)
    await rm(temporary, { recursive: true, force: true })
}
