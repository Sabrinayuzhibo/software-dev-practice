import { contextBridge, ipcRenderer } from 'electron'

contextBridge.exposeInMainWorld('backup', {
    getConfig: () => ipcRenderer.invoke('config:get'),
    saveTarget: (target: unknown) => ipcRenderer.invoke('targets:save', target),
    pingServer: (targetId = 'local') =>
        ipcRenderer.invoke('server:ping', targetId),
    listTasks: () => ipcRenderer.invoke('tasks:list'),
    addFolder: (targetId = 'local') =>
        ipcRenderer.invoke('tasks:add-folder', targetId),
    chooseSources: (kind: 'files' | 'folders') =>
        ipcRenderer.invoke('sources:choose', kind),
    previewSources: (
        paths: string[],
        targetId: string,
        fileTypes: string[],
        preserveEmptyDirs: boolean,
    ) =>
        ipcRenderer.invoke(
            'sources:preview',
            paths,
            targetId,
            fileTypes,
            preserveEmptyDirs,
        ),
    createTask: (
        paths: string[],
        targetId: string,
        name: string,
        fileTypes: string[],
        preserveEmptyDirs: boolean,
    ) =>
        ipcRenderer.invoke(
            'tasks:create',
            paths,
            targetId,
            name,
            fileTypes,
            preserveEmptyDirs,
        ),
    removeTask: (id: string) => ipcRenderer.invoke('tasks:remove', id),
    scanTask: (id: string) => ipcRenderer.invoke('tasks:scan', id),
    backupTask: (id: string) => ipcRenderer.invoke('tasks:backup', id),
    listVersions: (targetId: string, offset = 0, taskId = '') =>
        ipcRenderer.invoke('versions:list', targetId, offset, taskId),
    getVersionWarnings: (targetId: string, versionId: string, offset = 0) =>
        ipcRenderer.invoke('versions:warnings', targetId, versionId, offset),
    chooseRestoreDirectory: () =>
        ipcRenderer.invoke('directories:choose-restore'),
    restoreVersion: (
        targetId: string,
        versionId: string,
        destination: string,
    ) =>
        ipcRenderer.invoke(
            'versions:restore',
            targetId,
            versionId,
            destination,
        ),
    listOperations: (offset = 0, groupScans = false, scanGroup = '') =>
        ipcRenderer.invoke('operations:list', offset, groupScans, scanGroup),
    getOperation: (id: string, summary = false) =>
        ipcRenderer.invoke('operations:get', id, summary),
    getOperationWarnings: (id: string, offset = 0) =>
        ipcRenderer.invoke('operations:warnings', id, offset),
    getRestoreEntries: (id: string, offset = 0) =>
        ipcRenderer.invoke('operations:restore-entries', id, offset),
    confirmOperation: (id: string, targetId: string) =>
        ipcRenderer.invoke('operations:confirm', id, targetId),
    minimizeWindow: () => ipcRenderer.invoke('window:minimize'),
    toggleMaximizeWindow: () => ipcRenderer.invoke('window:toggle-maximize'),
    isWindowMaximized: () => ipcRenderer.invoke('window:is-maximized'),
    closeWindow: () => ipcRenderer.invoke('window:close'),
    onWindowMaximizedChange: (callback: (maximized: boolean) => void) => {
        const listener = (
            _event: Electron.IpcRendererEvent,
            maximized: boolean,
        ) => callback(maximized)
        ipcRenderer.on('window:maximized-changed', listener)
        return () =>
            ipcRenderer.removeListener('window:maximized-changed', listener)
    },
})
