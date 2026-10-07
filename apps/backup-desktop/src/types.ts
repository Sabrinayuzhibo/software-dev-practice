export interface BackupTask {
    id: string
    path: string
    target_id: string
    createdAt: string
}

export interface Target {
    id: string
    name: string
    host: string
    port: number
    mode?: 'local'
    repository_path?: string
    data_root?: string
}

export interface Configuration {
    tasks: BackupTask[]
    targets: Target[]
}

export interface Warning {
    path: string
    reason: string
    severity?: 'error'
}

export interface ScanResult {
    root: string
    files: number
    directories: number
    bytes: number
    complete: boolean
    warning_count: number
    error_count?: number
    sample: { path: string; type?: string; size?: number; sha256?: string }[]
}

export interface Version {
    id: string
    task_id: string
    source: string
    completed_at: string
    files: string
    directories: string
    bytes: string
    warning_count: number
    rules: Record<string, unknown>
}

export interface Operation {
    id: string
    action: string
    task_id?: string
    state: string
    stage: string
    started_at: string
    finished_at?: string
    source?: string
    destination?: string
    target: Target
    path?: string
    files: number
    directories?: number
    bytes: number
    error?: string
    warning_count: number
    error_count?: number
    complete?: boolean
    restore_journal?: boolean
    scan_group?: string
    scan_count?: number
    failure_count?: number
    warning_runs?: number
    result?: ScanResult & { version?: Version; destination?: string }
}

export interface OperationPage {
    operations: Operation[]
    total: number
    execution_total: number
    next_offset: number | null
}

export interface WarningPage {
    warnings: Warning[]
    total: number
    next_offset: number | null
}

export interface RestorePage {
    entries: {
        path: string
        type: string
        state: 'pending' | 'written'
        temporary_path?: string
    }[]
    total: number
    next_offset: number | null
    written: number
    uncertain: number
    incomplete: boolean
}

export interface DesktopApi {
    getConfig(): Promise<Configuration>
    saveTarget(target: Partial<Target>): Promise<Configuration>
    pingServer(targetId?: string): Promise<{
        connected: boolean
        storageAvailable?: boolean
        server?: string
        error?: string
        dataRoot?: string
    }>
    listTasks(): Promise<BackupTask[]>
    addFolder(targetId?: string): Promise<BackupTask | null>
    removeTask(id: string): Promise<void>
    scanTask(id: string): Promise<{ operation_id: string }>
    backupTask(id: string): Promise<{ operation_id: string }>
    listVersions(
        targetId: string,
        offset?: number,
        taskId?: string,
    ): Promise<{
        versions: Version[]
        total: number
        next_offset: number | null
    }>
    getVersionWarnings(
        targetId: string,
        versionId: string,
        offset?: number,
    ): Promise<WarningPage>
    restoreVersion(
        targetId: string,
        versionId: string,
        destination: string,
    ): Promise<{ operation_id: string } | null>
    chooseRestoreDirectory(): Promise<string | null>
    listOperations(
        offset?: number,
        groupScans?: boolean,
        scanGroup?: string,
    ): Promise<OperationPage>
    getOperation(id: string, summary?: boolean): Promise<Operation>
    getOperationWarnings(id: string, offset?: number): Promise<WarningPage>
    getRestoreEntries(id: string, offset?: number): Promise<RestorePage>
    confirmOperation(id: string, targetId: string): Promise<Operation>
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
