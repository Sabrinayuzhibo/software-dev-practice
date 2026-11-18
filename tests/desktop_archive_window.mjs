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
const temporary = await mkdtemp(join(tmpdir(), 'backup-archive-window-'))
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
try {
    const original = await server('original-repository')
    const target = await server('import-repository')
    const state = join(temporary, 'agent-state')
    await mkdir(state)
    await writeFile(
        join(state, 'config.json'),
        JSON.stringify({
            format: 7,
            tasks: [],
            targets: [
                {
                    id: 'local',
                    name: '归档原仓库',
                    host: '127.0.0.1',
                    port: original.port,
                    mode: 'local',
                    compression: 'huffman',
                },
            ],
        }),
    )
    const rendererPort = await port()
    let mainPort = await port()
    while (mainPort === rendererPort) mainPort = await port()
    const electron = launch(
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
    await main.evaluate(
        `globalThis.testElectron = process.getBuiltinModule('module').createRequire(process.cwd() + '/package.json')('electron'); globalThis.testWindow = testElectron.BrowserWindow.getAllWindows()[0]; testWindow.setSize(1120,760); testWindow.show();`,
    )
    await requireGlass(page, until)
    const source = join(temporary, 'source')
    const text = '独立归档窗口验证\n'.repeat(12000)
    await mkdir(join(source, 'empty'), { recursive: true })
    await writeFile(join(source, '中文.txt'), text)
    const task = await page.evaluate(
        `window.backup.createTask([${JSON.stringify(source)}], 'local', '归档样本', ['file'], true)`,
    )
    const start = await page.evaluate(`window.backup.backupTask('${task.id}')`)
    const backed = await operation(start.operation_id)
    const sourceId = backed.result.version.id
    await page.call('Page.reload')
    await requireGlass(page, until)
    await click('[data-tab="versions"]')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.versions-table tbody tr').length === 1",
            ),
        'source version',
    )
    const archive = join(temporary, '独立归档 file.bsar')
    await main.evaluate(
        'testElectron.dialog.showSaveDialog = async () => ({canceled:true})',
    )
    const beforeCancel = await page.evaluate('window.backup.listOperations()')
    await click('.export-archive')
    await until(
        () =>
            page.evaluate(
                "!document.querySelector('.export-archive').disabled",
            ),
        'export cancellation',
    )
    assert.equal(
        (await page.evaluate('window.backup.listOperations()')).execution_total,
        beforeCancel.execution_total,
    )
    await main.evaluate(
        `testElectron.dialog.showSaveDialog = async () => ({canceled:false,filePath:${JSON.stringify(archive)}})`,
    )
    await click('.export-archive')
    await finished('导出归档')
    const archiveBytes = await readFile(archive)
    assert.equal(archiveBytes.subarray(0, 4).toString(), 'BSAR')
    assert(
        await page.evaluate(
            "document.querySelector('.archive-path')?.textContent.includes('独立归档 file.bsar')",
        ),
    )
    await click('.export-archive')
    await finished('导出归档', false)
    assert.deepEqual(await readFile(archive), archiveBytes)
    await page.evaluate(
        `window.backup.saveTarget({id:'archive-target',name:'归档导入仓库',host:'127.0.0.1',port:${target.port},compression:'lz77'})`,
    )
    await page.call('Page.reload')
    await requireGlass(page, until)
    await page.evaluate(
        "(() => { const element=document.querySelector('.target-selector select'); element.value='archive-target';element.dispatchEvent(new Event('change',{bubbles:true})); })()",
    )
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.status-dot')?.classList.contains('online') && !document.querySelector('.target-selector select').disabled",
            ),
        'new repository connected',
    )
    await click('[data-tab="versions"]')
    original.child.kill('SIGTERM')
    await until(
        () =>
            original.child.exitCode !== null ||
            original.child.signalCode !== null,
        'source server stopped',
    )
    await rename(
        join(temporary, 'original-repository'),
        join(temporary, 'unavailable-original'),
    )
    await rename(source, join(temporary, 'unavailable-source'))
    await main.evaluate(
        'testElectron.dialog.showOpenDialog = async () => ({canceled:true,filePaths:[]})',
    )
    const beforeImport = await page.evaluate('window.backup.listOperations()')
    await click('.import-archive')
    await until(
        () =>
            page.evaluate(
                "!document.querySelector('.import-archive').disabled",
            ),
        'import cancellation',
    )
    assert.equal(
        (await page.evaluate('window.backup.listOperations()')).execution_total,
        beforeImport.execution_total,
    )
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({canceled:false,filePaths:[${JSON.stringify(archive)}]})`,
    )
    await click('.import-archive')
    await finished('导入归档')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.versions-table tbody tr').length === 1",
            ),
        'imported version',
    )
    const importedId = await page.evaluate(
        "document.querySelector('.versions-table tbody tr').dataset.versionId",
    )
    assert.notEqual(importedId, sourceId)
    const destination = join(temporary, 'restored')
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({canceled:false,filePaths:[${JSON.stringify(destination)}]})`,
    )
    await click('.versions-table tbody .version-actions button')
    await click('[aria-label="选择还原目录"]')
    await click('.confirm-restore')
    await finished('还原')
    assert.equal(await readFile(join(destination, '中文.txt'), 'utf8'), text)
    const broken = join(temporary, 'broken.bsar')
    const damaged = Buffer.from(archiveBytes)
    damaged[damaged.length - 1] ^= 1
    await writeFile(broken, damaged)
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({canceled:false,filePaths:[${JSON.stringify(broken)}]})`,
    )
    await click('.import-archive')
    await finished('导入归档', false)
    assert.equal(
        await page.evaluate(
            "document.querySelectorAll('.versions-table tbody tr').length",
        ),
        1,
    )
    for (const [width, height] of [
        [1120, 760],
        [850, 600],
    ]) {
        await main.evaluate(`testWindow.setSize(${width},${height})`)
        await delay(200)
        const geometry = await page.evaluate(
            `({width:innerWidth,scroll:document.documentElement.scrollWidth,tables:[...document.querySelectorAll('.table-scroll')].every(e=>e.scrollWidth<=e.clientWidth)})`,
        )
        assert(
            geometry.scroll <= geometry.width && geometry.tables,
            JSON.stringify(geometry),
        )
        const capture = await page.call('Page.captureScreenshot', {
            format: 'png',
        })
        await writeFile(
            join(tmpdir(), `backup-archive-${width}.png`),
            Buffer.from(capture.data, 'base64'),
        )
    }
    await page.call('Page.reload')
    await requireGlass(page, until)
    const operations = await page.evaluate('window.backup.listOperations()')
    assert(
        operations.operations.some(
            (item) =>
                item.action === 'import_archive' && item.state === 'SUCCEEDED',
        ),
    )
    assert(
        operations.operations.some(
            (item) =>
                item.action === 'export_archive' &&
                item.archive_path === archive,
        ),
    )
    // 独立的新仓库覆盖真实密码控件、锁定和加密归档操作。
    const encrypted = await server('encrypted-repository')
    await page.evaluate(
        `window.backup.saveTarget({id:'encrypted',name:'加密验收仓库',host:'127.0.0.1',port:${encrypted.port}})`,
    )
    await page.call('Page.reload')
    await requireGlass(page, until)
    await page.evaluate(`(() => {
        const select = document.querySelector('.target-selector select');
        select.value = 'encrypted';
        select.dispatchEvent(new Event('change', { bubbles: true }));
    })()`)
    await click('[data-tab="targets"]')
    const row = 'tr[data-target-id="encrypted"]'
    const typePassword = async (index, value) => {
        await page.evaluate(`(() => {
            const input = document.querySelectorAll('dialog[open] input[type="password"]')[${index}];
            input.focus();
            input.select();
        })()`)
        await page.call('Input.insertText', { text: value })
    }
    const testPassword = 'fixture-password-非生产!'
    await click(row + ' .enable-encryption')
    await typePassword(0, testPassword)
    await typePassword(1, 'wrong confirmation')
    await click('dialog[open] button[type="submit"]')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('dialog[open] [role=alert]')?.textContent.includes('不一致')",
            ),
        'password confirmation',
    )
    await typePassword(1, testPassword)
    await click('dialog[open] button[type="submit"]')
    await until(
        () => page.evaluate("!document.querySelector('dialog[open]')"),
        'encryption enabled',
    )
    await click(row + ' .lock-repository')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.status-dot')?.classList.contains('locked')",
            ),
        'locked status',
    )
    await click(row + ' .unlock-repository')
    await typePassword(0, 'wrong')
    await click('dialog[open] button[type="submit"]')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('dialog[open] [role=alert]')?.textContent.includes('密码错误')",
            ),
        'wrong password rejected',
    )
    await typePassword(0, testPassword)
    await click('dialog[open] button[type="submit"]')
    await until(
        () =>
            page.evaluate(
                "!document.querySelector('dialog[open]') && document.querySelector('.status-dot')?.classList.contains('online')",
            ),
        'correct password unlocks',
    )
    await click('[data-tab="versions"]')
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({canceled:false,filePaths:[${JSON.stringify(archive)}]})`,
    )
    await click('.import-archive')
    await finished('导入归档')
    const sealedArchive = join(temporary, 'encrypted.bsar')
    await main.evaluate(
        `testElectron.dialog.showSaveDialog = async () => ({canceled:false,filePath:${JSON.stringify(sealedArchive)}})`,
    )
    await click('.export-archive')
    await finished('导出归档')
    assert.equal(
        (await readFile(sealedArchive)).subarray(0, 4).toString(),
        'BSAE',
    )
    await main.evaluate(
        `testElectron.dialog.showOpenDialog = async () => ({canceled:false,filePaths:[${JSON.stringify(sealedArchive)}]})`,
    )
    await click('.import-encrypted-archive')
    await typePassword(0, testPassword)
    await click('dialog[open] button[type="submit"]')
    await finished('导入归档')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.versions-table tbody tr').length === 2",
            ),
        'encrypted archive imported in GUI',
    )
    console.log(
        'PASS: real password dialog, mismatch and wrong password, lock/unlock, encrypted export and import',
    )
    await main.evaluate('testWindow.close()')
    main.close()
    page.close()
    await until(() => electron.exitCode !== null, 'window exit')
    assert.equal(electron.exitCode, 0)
    console.log(
        'PASS: real archive picker/buttons, cancellations, protected export, offline original, codec-converting import/restore, corrupt archive, persisted records and 1120/850 layouts',
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
    for (const child of children.reverse()) {
        if (child.exitCode === null) {
            try {
                process.kill(-child.pid, 'SIGTERM')
            } catch {}
        }
    }
    // 桌面退出后服务仍常驻；测试必须清理自己隔离目录中的实例。
    try {
        const lock = await readFile(
            join(temporary, 'agent-state/agent.lock'),
            'utf8',
        )
        process.kill(Number(lock.split('\n')[0]), 'SIGTERM')
    } catch {}
    await delay(300)
    await rm(temporary, { recursive: true, force: true })
}
