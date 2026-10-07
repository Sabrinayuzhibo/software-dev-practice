// Privileged desktop adapter: native dialogs and a fixed Agent command surface.
import { app, BrowserWindow, dialog, ipcMain, Menu } from 'electron'
import { promises as fs } from 'node:fs'
import { join, resolve } from 'node:path'
import { AgentBridge } from './agent-bridge'

let bridge: AgentBridge
let mainWindow: BrowserWindow | null = null

function registerHandlers(): void {
    const windowFor = (sender: Electron.WebContents): BrowserWindow => {
        const window = BrowserWindow.fromWebContents(sender)
        if (!window || window !== mainWindow) {
            throw new Error('窗口不可用')
        }
        return window
    }
    ipcMain.handle('window:minimize', (event) =>
        windowFor(event.sender).minimize(),
    )
    ipcMain.handle('window:toggle-maximize', (event) => {
        const window = windowFor(event.sender)
        if (window.isMaximized()) {
            window.unmaximize()
        } else {
            window.maximize()
        }
    })
    ipcMain.handle('window:is-maximized', (event) =>
        windowFor(event.sender).isMaximized(),
    )
    ipcMain.handle('window:close', (event) => windowFor(event.sender).close())
    ipcMain.handle('config:get', () => bridge.send('config', {}))
    ipcMain.handle('targets:save', (_event, target: unknown) => {
        if (!target || typeof target !== 'object') {
            throw new Error('目标配置无效')
        }
        return bridge.send('save_target', target)
    })
    ipcMain.handle('server:ping', async (_event, targetId = 'local') => {
        try {
            const result = await bridge.send<{
                server: string
                data_root: string
                storage_state: string
                storage_error?: string
            }>('ping', { target_id: targetId }, 15000)
            return {
                connected: true,
                storageAvailable: result.storage_state === 'available',
                error: result.storage_error,
                server: result.server,
                dataRoot: result.data_root,
            }
        } catch (error) {
            return { connected: false, error: String(error) }
        }
    })
    ipcMain.handle('tasks:list', async () => {
        const result = await bridge.send<{ tasks: unknown[] }>('config', {})
        return result.tasks
    })
    ipcMain.handle('tasks:add-folder', async (_event, targetId = 'local') => {
        const selection = await dialog.showOpenDialog(mainWindow!, {
            properties: ['openDirectory'],
            title: '选择需要备份的目录',
        })
        if (selection.canceled || selection.filePaths.length === 0) {
            return null
        }
        return bridge.send('add_task', {
            path: selection.filePaths[0],
            target_id: targetId,
        })
    })
    ipcMain.handle('tasks:remove', (_event, id: unknown) =>
        bridge.send('remove_task', { id }),
    )
    ipcMain.handle('tasks:scan', (_event, id: unknown) =>
        bridge.send('start_scan', { task_id: id }),
    )
    ipcMain.handle('tasks:backup', (_event, id: unknown) =>
        bridge.send('start_backup', { task_id: id }),
    )
    ipcMain.handle(
        'versions:list',
        (_event, targetId: unknown, offset = 0, taskId = '') =>
            bridge.send(
                'versions',
                { target_id: targetId, offset, task_id: taskId },
                15000,
            ),
    )
    ipcMain.handle('directories:choose-restore', async () => {
        const selection = await dialog.showOpenDialog(mainWindow!, {
            properties: ['openDirectory', 'createDirectory'],
            title: '选择新建或空的还原目录',
        })
        if (selection.canceled || selection.filePaths.length === 0) {
            return null
        }
        return selection.filePaths[0]
    })
    ipcMain.handle(
        'versions:warnings',
        (_event, targetId: unknown, versionId: unknown, offset = 0) =>
            bridge.send('version_warnings', {
                target_id: targetId,
                version_id: versionId,
                offset,
            }),
    )
    ipcMain.handle(
        'versions:restore',
        (
            _event,
            targetId: unknown,
            versionId: unknown,
            destination: unknown,
        ) => {
            if (typeof destination !== 'string' || !destination) {
                throw new Error('请选择还原目录')
            }
            return bridge.send('start_restore', {
                target_id: targetId,
                version_id: versionId,
                destination,
            })
        },
    )
    ipcMain.handle(
        'operations:list',
        (_event, offset = 0, groupScans = false, scanGroup = '') =>
            bridge.send('operations', {
                offset,
                group_scans: groupScans,
                scan_group: scanGroup,
            }),
    )
    ipcMain.handle('operations:get', (_event, id: unknown, summary = false) =>
        bridge.send('operation', { id, summary }),
    )
    ipcMain.handle('operations:warnings', (_event, id: unknown, offset = 0) =>
        bridge.send('operation_warnings', { id, offset }),
    )
    ipcMain.handle(
        'operations:restore-entries',
        (_event, id: unknown, offset = 0) =>
            bridge.send('restore_entries', { id, offset }),
    )
    ipcMain.handle(
        'operations:confirm',
        (_event, id: unknown, targetId: unknown) =>
            bridge.send('confirm', { id, target_id: targetId }, 15000),
    )
}

async function migrateTasks(): Promise<void> {
    const config = await bridge.send<{ legacy_imported?: boolean }>(
        'config',
        {},
    )
    if (config.legacy_imported) {
        return
    }
    let tasks: unknown = []
    try {
        tasks = JSON.parse(
            await fs.readFile(
                join(app.getPath('userData'), 'tasks.json'),
                'utf8',
            ),
        )
    } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== 'ENOENT') {
            throw error
        }
    }
    await bridge.send('import_tasks', { tasks })
}

function createWindow(): void {
    mainWindow = new BrowserWindow({
        width: 1120,
        height: 760,
        minWidth: 850,
        minHeight: 600,
        title: 'Backup System',
        frame: false,
        // Avoid a second set of Linux decoration/input insets in WSLg.
        ...(process.platform === 'linux'
            ? { roundedCorners: false, hasShadow: false }
            : {}),
        autoHideMenuBar: true,
        backgroundColor: '#f7f8fa',
        webPreferences: {
            preload: resolve(__dirname, 'preload.js'),
            contextIsolation: true,
            nodeIntegration: false,
            // Keep progress polling alive when WSLg temporarily reports the
            // restored surface as occluded/hidden.
            backgroundThrottling: false,
        },
    })
    const notifyMaximized = () => {
        const window = mainWindow
        if (window) {
            window.webContents.send(
                'window:maximized-changed',
                window.isMaximized(),
            )
        }
    }
    mainWindow.on('maximize', notifyMaximized)
    mainWindow.on('unmaximize', notifyMaximized)
    const window = mainWindow
    const resumeContent = () => {
        if (window.isDestroyed()) {
            return
        }
        window.webContents.invalidate()
        if (window.isFocused()) {
            window.webContents.focus()
        }
        notifyMaximized()
    }
    window.on('restore', () => {
        window.focus()
        resumeContent()
    })
    window.on('show', resumeContent)
    window.on('focus', () => {
        // The host taskbar can activate a WSLg surface before Electron has
        // received its restore event. Reconcile the native minimized state.
        if (window.isMinimized()) {
            window.restore()
        }
        resumeContent()
    })
    void mainWindow.loadFile(resolve(__dirname, '../dist-renderer/index.html'))
    mainWindow.on('closed', () => {
        mainWindow = null
    })
}

app.whenReady().then(async () => {
    try {
        Menu.setApplicationMenu(null)
        bridge = new AgentBridge(
            process.env.BACKUP_AGENT_STATE_DIR ||
                join(app.getPath('userData'), 'agent'),
        )
        await migrateTasks()
        registerHandlers()
        createWindow()
    } catch (error) {
        dialog.showErrorBox('Backup System 启动失败', String(error))
        app.quit()
    }
})
app.on('window-all-closed', () => {
    if (process.platform !== 'darwin') {
        app.quit()
    }
})
app.on('activate', () => {
    if (!mainWindow) {
        createWindow()
    } else {
        if (mainWindow.isMinimized()) {
            mainWindow.restore()
        }
        mainWindow.show()
        mainWindow.focus()
    }
})
app.on('before-quit', () => bridge?.stop())
