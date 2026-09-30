import { contextBridge, ipcRenderer } from 'electron'

contextBridge.exposeInMainWorld('backup', {
  pingServer: () => ipcRenderer.invoke('server:ping'),
  listTasks: () => ipcRenderer.invoke('tasks:list'),
  addFolder: () => ipcRenderer.invoke('tasks:add-folder'),
  removeTask: (id: string) => ipcRenderer.invoke('tasks:remove', id),
  scanTask: (id: string) => ipcRenderer.invoke('tasks:scan', id),
  minimizeWindow: () => ipcRenderer.invoke('window:minimize'),
  toggleMaximizeWindow: () => ipcRenderer.invoke('window:toggle-maximize'),
  isWindowMaximized: () => ipcRenderer.invoke('window:is-maximized'),
  closeWindow: () => ipcRenderer.invoke('window:close'),
  onWindowMaximizedChange: (callback: (maximized: boolean) => void) => {
    const listener = (_event: Electron.IpcRendererEvent, maximized: boolean) => callback(maximized)
    ipcRenderer.on('window:maximized-changed', listener)
    return () => ipcRenderer.removeListener('window:maximized-changed', listener)
  },
})
