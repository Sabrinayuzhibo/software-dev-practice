import { registerArchiveHandlers } from './archive-handlers'
// Privileged desktop adapter: native dialogs and a fixed Agent command surface.
import { app, BrowserWindow, dialog, ipcMain, Menu } from 'electron'
import { promises as fs } from 'node:fs'
import { join, resolve } from 'node:path'
import { AgentBridge } from './agent-bridge'

// Select Electron's bundled CPU driver before Chromium creates any GPU process.
// Keep this here so both the launcher and direct Electron starts use WebGL.
app.commandLine.appendSwitch('use-gl', 'angle')
app.commandLine.appendSwitch('use-angle', 'swiftshader')
app.commandLine.appendSwitch('enable-unsafe-swiftshader')

let bridge: AgentBridge
let mainWindow: BrowserWindow | null = null

function registerHandlers(): void {
    registerArchiveHandlers(bridge, () => {
        if (!mainWindow) {
            throw new Error('窗口不可用')
        }
        return mainWindow
    })
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
    ipcMain.handle(
        'targets:encryption',
        (_event, command: unknown, id: unknown, password: unknown) => {
            const actions = new Map([
                ['create', 'initialize_encryption'],
                ['unlock', 'unlock_repository'],
                ['lock', 'lock_repository'],
            ])
            const action =
                typeof command === 'string' ? actions.get(command) : undefined
            if (
                !action ||
                typeof id !== 'string' ||
                !id ||
                id.length > 120 ||
                (command !== 'lock' &&
                    (typeof password !== 'string' ||
                        Buffer.byteLength(password) > 1024))
            ) {
                throw new Error('仓库密码设置无效')
            }
            const payload =
                command === 'lock'
                    ? { target_id: id }
                    : { target_id: id, password }
            return bridge.send(action, payload, 20000)
        },
    )
    ipcMain.handle('server:ping', async (_event, targetId = 'local') => {
        try {
            const result = await bridge.send<{
                server: string
                data_root: string
                storage_state: string
                storage_error?: string
                repository_locked?: boolean
            }>('ping', { target_id: targetId }, 15000)
            return {
                connected: true,
                storageAvailable: result.storage_state === 'available',
                error: result.storage_error,
                server: result.server,
                dataRoot: result.data_root,
                locked: result.repository_locked,
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
    ipcMain.handle('sources:choose', async (_event, kind: unknown) => {
        if (kind !== 'files' && kind !== 'folders') {
            throw new Error('来源类型无效')
        }
        const selection = await dialog.showOpenDialog(mainWindow!, {
            properties: [
                kind === 'files' ? 'openFile' : 'openDirectory',
                'multiSelections',
            ],
            title:
                kind === 'files'
                    ? '选择需要备份的文件'
                    : '选择需要备份的文件夹',
        })
        return selection.canceled ? null : selection.filePaths
    })
    // 主进程限制 IPC 体积，规则字段及数值边界仍由 Agent 最终校验。
    const checkedFilters = (filters: unknown) => {
        if (
            !filters ||
            typeof filters !== 'object' ||
            Array.isArray(filters) ||
            Buffer.byteLength(JSON.stringify(filters), 'utf8') > 16 * 1024
        ) {
            throw new Error('自定义筛选规则无效或过大')
        }
        return filters
    }
    const sourceArgs = (
        paths: unknown,
        targetId: unknown,
        fileTypes: unknown,
        preserveEmptyDirs: unknown,
        name?: unknown,
        filters?: unknown,
    ) => {
        const supported = [
            'file',
            'symlink',
            'fifo',
            'character_device',
            'block_device',
        ]
        if (
            !Array.isArray(paths) ||
            paths.length < 1 ||
            paths.length > 100 ||
            paths.some(
                (path) =>
                    typeof path !== 'string' || !path || path.length > 4096,
            ) ||
            typeof targetId !== 'string' ||
            !Array.isArray(fileTypes) ||
            fileTypes.length < 1 ||
            fileTypes.length > supported.length ||
            fileTypes.some((type) => !supported.includes(type)) ||
            new Set(fileTypes).size !== fileTypes.length ||
            typeof preserveEmptyDirs !== 'boolean' ||
            (name !== undefined && typeof name !== 'string')
        ) {
            throw new Error('请选择有效来源和至少一种可备份类型')
        }
        return {
            sources: paths,
            target_id: targetId,
            file_types: fileTypes,
            preserve_empty_dirs: preserveEmptyDirs,
            ...(name !== undefined ? { name } : {}),
            ...(filters !== undefined
                ? { filters: checkedFilters(filters) }
                : {}),
        }
    }
    ipcMain.handle(
        'sources:preview',
        (
            _event,
            paths: unknown,
            targetId: unknown,
            fileTypes: unknown,
            preserveEmptyDirs: unknown,
            filters?: unknown,
        ) =>
            bridge.send(
                'preview_sources',
                sourceArgs(
                    paths,
                    targetId,
                    fileTypes,
                    preserveEmptyDirs,
                    undefined,
                    filters,
                ),
            ),
    )
    ipcMain.handle(
        'tasks:create',
        (
            _event,
            paths: unknown,
            targetId: unknown,
            name: unknown,
            fileTypes: unknown,
            preserveEmptyDirs: unknown,
            filters?: unknown,
        ) =>
            bridge.send(
                'add_task',
                sourceArgs(
                    paths,
                    targetId,
                    fileTypes,
                    preserveEmptyDirs,
                    name,
                    filters,
                ),
            ),
    )
    for (const [channel, action] of [
        ['tasks:update-filters', 'update_task_filters'],
        ['tasks:preview-filters', 'start_filter_preview'],
    ]) {
        ipcMain.handle(channel, (_event, id: unknown, filters: unknown) => {
            if (typeof id !== 'string' || !id || id.length > 120) {
                throw new Error('任务标识无效')
            }
            return bridge.send(action, {
                ...(action === 'update_task_filters'
                    ? { id }
                    : { task_id: id }),
                filters: checkedFilters(filters),
            })
        })
    }
    ipcMain.handle(
        'tasks:realtime',
        (_event, id: unknown, enabled: unknown) => {
            if (
                typeof id !== 'string' ||
                !id ||
                id.length > 120 ||
                typeof enabled !== 'boolean'
            ) {
                throw new Error('实时备份设置无效')
            }
            return bridge.send('set_task_realtime', { id, enabled })
        },
    )
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
        'versions:delete',
        (
            _event,
            targetId: unknown,
            versionId: unknown,
            repositoryPath: unknown,
        ) => {
            if (
                typeof targetId !== 'string' ||
                !targetId ||
                typeof versionId !== 'string' ||
                !/^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$/.test(versionId) ||
                typeof repositoryPath !== 'string' ||
                !repositoryPath
            ) {
                throw new Error('请重新选择要删除的备份版本')
            }
            return bridge.send('start_delete_version', {
                target_id: targetId,
                version_id: versionId,
                repository_path: repositoryPath,
            })
        },
    )
    ipcMain.handle('operations:retry-delete', (_event, id: unknown) => {
        if (typeof id !== 'string' || !id) {
            throw new Error('缺少删除执行记录')
        }
        return bridge.send('start_delete_version', { retry_operation_id: id })
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
    // Only the bundled application may submit shaders to the software driver.
    mainWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }))
    mainWindow.webContents.on('will-navigate', (event) =>
        event.preventDefault(),
    )
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
