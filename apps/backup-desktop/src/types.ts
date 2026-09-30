export interface BackupTask {
  id: string
  path: string
  createdAt: string
}

export interface ScanFile {
  path: string
  size: number
  sha256: string
  modified_at: string
}

export interface ScanResult {
  root: string
  files: number
  directories: number
  bytes: number
  hashed_files: number
  unreadable_files: number
  sample: ScanFile[]
}

export interface DesktopApi {
  pingServer(): Promise<{ connected: boolean; server?: string; error?: string }>
  listTasks(): Promise<BackupTask[]>
  addFolder(): Promise<BackupTask | null>
  removeTask(id: string): Promise<void>
  scanTask(id: string): Promise<ScanResult>
  minimizeWindow(): Promise<void>
  toggleMaximizeWindow(): Promise<void>
  isWindowMaximized(): Promise<boolean>
  closeWindow(): Promise<void>
  onWindowMaximizedChange(callback: (maximized: boolean) => void): () => void
}

declare global {
  interface Window {
    backup: DesktopApi
  }
}
