// Runs the actual Electron window. Requires the same display session as run-agent.sh.
// Mouse events go through Chromium input routing, not HTMLElement.click().
import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { mkdtemp, rm } from 'node:fs/promises'
import { createServer } from 'node:net'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { setTimeout as delay } from 'node:timers/promises'

const root = fileURLToPath(new URL('../', import.meta.url))
const temporary = await mkdtemp(join(tmpdir(), 'backup-window-test-'))

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
  await until(async () => {
    try {
      const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json()
      url = targets[0]?.webSocketDebuggerUrl
      return !!url
    } catch { return false }
  }, 'Electron debugger startup', 30000)
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
    if (message.error) request.reject(new Error(JSON.stringify(message.error)))
    else request.resolve(message.result)
  }
  socket.onclose = () => {
    for (const request of pending.values()) {
      clearTimeout(request.timer)
      request.reject(new Error('Debugger connection closed'))
    }
    pending.clear()
  }
  const call = (method, params = {}) => new Promise((resolve, reject) => {
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
        expression, returnByValue: true, awaitPromise: true,
      })
      if (response.exceptionDetails) throw new Error(JSON.stringify(response.exceptionDetails))
      return response.result.value
    },
    close: () => socket.close(),
  }
}

const rendererPort = await freePort()
const mainPort = await freePort()
const child = spawn(join(root, 'scripts/run-agent.sh'), [
  `--remote-debugging-port=${rendererPort}`, `--inspect=127.0.0.1:${mainPort}`,
  `--user-data-dir=${join(temporary, 'chromium')}`,
], {
  cwd: root,
  env: { ...process.env, XDG_CONFIG_HOME: join(temporary, 'config') },
  detached: true,
  stdio: ['ignore', 'pipe', 'pipe'],
})
let output = ''
for (const stream of [child.stdout, child.stderr]) {
  stream.on('data', (data) => { output = (output + data).slice(-12000) })
}
let page
let main
try {
  page = await connect(rendererPort)
  main = await connect(mainPort)
  await until(() => page.evaluate("document.querySelectorAll('.window-control').length === 3"), 'React title bar')
  await main.evaluate(`
    globalThis.testElectron = process.mainModule.require('electron');
    globalThis.testWindow = testElectron.BrowserWindow.getAllWindows()[0];
    globalThis.folderRequests = 0;
    // Test the full button -> preload -> IPC path without leaving a modal OS picker open.
    testElectron.dialog.showOpenDialog = async () => {
      folderRequests++;
      return { canceled: true, filePaths: [] };
    };
    testWindow.restore();
    if (testWindow.isMaximized()) testWindow.unmaximize();
  `)
  await until(() => main.evaluate('!testWindow.isMaximized()'), 'initial restore')
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
      const r = element.getBoundingClientRect();
      return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
    })()`)
    await page.call('Input.dispatchMouseEvent', { type: 'mouseMoved', ...point })
    await page.call('Input.dispatchMouseEvent', { type: 'mousePressed', button: 'left', buttons: 1, clickCount: 1, ...point })
    await page.call('Input.dispatchMouseEvent', { type: 'mouseReleased', button: 'left', buttons: 0, clickCount: 1, ...point })
  }

  async function refresh() {
    const before = await page.evaluate('windowTestClicks.length')
    await click('.text-button')
    await until(async () => {
      const events = await page.evaluate('windowTestClicks')
      return events.length > before && events.at(-1).label === 'text-button' && events.at(-1).trusted
    }, 'refresh mouse input')
    await until(() => page.evaluate("!document.querySelector('.status-dot').classList.contains('checking')"), 'server response')
  }

  for (let cycle = 0; cycle < 5; cycle++) {
    await click('.window-control:nth-child(2)')
    await until(() => main.evaluate('testWindow.isMaximized()'), 'maximize')
    await until(() => page.evaluate("document.querySelector('.window-control:nth-child(2)').ariaLabel === '还原'"), 'maximized icon')
    await refresh()
    const before = await main.evaluate('folderRequests')
    await click('.primary-button')
    await until(() => main.evaluate(`folderRequests === ${before + 1}`), 'directory chooser IPC')
    await click('.window-control:nth-child(2)')
    await until(() => main.evaluate('!testWindow.isMaximized()'), 'restore')
    await until(() => page.evaluate("document.querySelector('.window-control:nth-child(2)').ariaLabel === '最大化'"), 'restored icon')
    await refresh()
  }
  console.log('PASS: 5 maximize/restore cycles; refresh and directory entry remained responsive')
  await click('.window-control:first-child')
  await until(() => main.evaluate('testWindow.isMinimized()'), 'minimize')
  await until(() => page.evaluate("document.visibilityState === 'hidden'"), 'minimized visibility')
  await main.evaluate('testWindow.restore(); testWindow.show()')
  await until(() => page.evaluate("document.visibilityState === 'visible'"), 'restore visibility')
  await refresh()
  console.log('PASS: minimize, restore and refresh')
  try { await click('.window-control.close') } catch (error) {
    if (!/connection closed|Cannot find context/.test(String(error))) throw error
  }
  main.close()
  page.close()
  await until(() => child.exitCode !== null, 'application exit')
  assert.equal(child.exitCode, 0)
  console.log('PASS: 5 maximize/restore cycles, 11 refresh clicks, 5 directory requests, minimize/restore, close')
} catch (error) {
  console.error(output)
  throw error
} finally {
  page?.close()
  main?.close()
  if (child.exitCode === null) {
    try { process.kill(-child.pid, 'SIGTERM') } catch {}
    await delay(300)
  }
  await rm(temporary, { recursive: true, force: true })
}
