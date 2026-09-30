import { app, BrowserWindow, dialog, ipcMain, Menu } from 'electron'
import { randomUUID } from 'node:crypto'
import { promises as fs } from 'node:fs'
import { join, resolve } from 'node:path'
import { AgentBridge } from './agent-bridge'

interface BackupTask {
  id: string
  path: string
  createdAt: string
}

const bridge = new AgentBridge()
let tasks: BackupTask[] = []
let mainWindow: BrowserWindow | null = null

function taskFile(): string {
  return join(app.getPath('userData'), 'tasks.json')
}

async function loadTasks(): Promise<BackupTask[]> {
  try {
    const content = await fs.readFile(taskFile(), 'utf8')
    const parsed: unknown = JSON.parse(content)
    if (!Array.isArray(parsed) || !parsed.every((item) =>
      typeof item?.id === 'string' &&
      typeof item?.path === 'string' &&
      typeof item?.createdAt === 'string')) {
      throw new Error('任务文件格式不正确')
    }
    return parsed as BackupTask[]
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return []
    throw error
  }
}

async function saveTasks(): Promise<void> {
  await fs.mkdir(app.getPath('userData'), { recursive: true })
  const temporary = taskFile() + '.tmp'
  await fs.writeFile(temporary, JSON.stringify(tasks, null, 2), 'utf8')
  await fs.rename(temporary, taskFile())
}

function requireTask(id: unknown): BackupTask {
  if (typeof id !== 'string') throw new Error('任务 ID 无效')
  const task = tasks.find((item) => item.id === id)
  if (!task) throw new Error('任务不存在')
  return task
}

function registerHandlers(): void {
  const windowFor = (sender: Electron.WebContents): BrowserWindow => {
    const window = BrowserWindow.fromWebContents(sender)
    if (!window || window !== mainWindow) throw new Error('窗口不可用')
    return window
  }
  ipcMain.handle('window:minimize', (event) => windowFor(event.sender).minimize())
  ipcMain.handle('window:toggle-maximize', (event) => {
    const window = windowFor(event.sender)
    if (window.isMaximized()) window.unmaximize()
    else window.maximize()
  })
  ipcMain.handle('window:is-maximized', (event) => windowFor(event.sender).isMaximized())
  ipcMain.handle('window:close', (event) => windowFor(event.sender).close())
  ipcMain.handle('server:ping', async () => {
    try {
      const result = await bridge.send<{ server: string }>('ping',
        { host: '127.0.0.1', port: 9000 })
      return { connected: true, server: result.server }
    } catch (error) {
      return { connected: false, error: error instanceof Error ? error.message : String(error) }
    }
  })
  ipcMain.handle('tasks:list', () => tasks)
  ipcMain.handle('tasks:add-folder', async () => {
    const selection = await dialog.showOpenDialog(mainWindow!, {
      properties: ['openDirectory'],
      title: '选择需要备份的目录',
    })
    if (selection.canceled || selection.filePaths.length === 0) return null
    const path = await fs.realpath(selection.filePaths[0])
    const info = await fs.stat(path)
    if (!info.isDirectory()) throw new Error('请选择目录')
    const existing = tasks.find((task) => task.path === path)
    if (existing) return existing
    const task = { id: randomUUID(), path, createdAt: new Date().toISOString() }
    tasks.push(task)
    try {
      await saveTasks()
    } catch (error) {
      tasks.pop()
      throw error
    }
    return task
  })
  ipcMain.handle('tasks:remove', async (_event, id: unknown) => {
    const task = requireTask(id)
    tasks = tasks.filter((item) => item.id !== task.id)
    try {
      await saveTasks()
    } catch (error) {
      tasks.push(task)
      throw error
    }
  })
  ipcMain.handle('tasks:scan', async (_event, id: unknown) => {
    const task = requireTask(id)
    return bridge.send('scan', { path: task.path }, 5 * 60 * 1000)
  })
}

function createWindow(): void {
  mainWindow = new BrowserWindow({
    width: 1120,
    height: 760,
    minWidth: 850,
    minHeight: 600,
    title: 'Backup System',
    frame: false,
    // Electron 43+ adds Linux client-side decoration insets to frameless windows.
    // WSLg already manages the outer window; avoid two sets of resize/input bounds.
    ...(process.platform === 'linux' ? { roundedCorners: false, hasShadow: false } : {}),
    autoHideMenuBar: true,
    backgroundColor: '#f7f9fc',
    webPreferences: {
      preload: resolve(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  })
  const notifyMaximized = () => {
    const window = mainWindow
    if (window) window.webContents.send('window:maximized-changed', window.isMaximized())
  }
  mainWindow.on('maximize', notifyMaximized)
  mainWindow.on('unmaximize', notifyMaximized)
  void mainWindow.loadFile(resolve(__dirname, '../dist-renderer/index.html'))
  mainWindow.on('closed', () => { mainWindow = null })
}

app.whenReady().then(async () => {
  try {
    Menu.setApplicationMenu(null)
    tasks = await loadTasks()
    registerHandlers()
    createWindow()
  } catch (error) {
    dialog.showErrorBox('Backup System 启动失败', String(error))
    app.quit()
  }
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit()
})
app.on('activate', () => {
  if (!mainWindow) createWindow()
})
app.on('before-quit', () => bridge.stop())
