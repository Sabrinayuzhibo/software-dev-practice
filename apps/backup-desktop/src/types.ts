export type FilterCategory =
    | 'path'
    | 'type'
    | 'name'
    | 'mtime'
    | 'size'
    | 'user'
export interface FilterRule {
    category: FilterCategory
    pattern?: string
    value?: string
    min?: string
    max?: string
}
export interface BackupFilters {
    include: FilterRule[]
    exclude: FilterRule[]
}

export interface BackupTask {
    realtime?: boolean
    id: string
    path: string
    target_id: string
    createdAt: string
    name?: string
    selection?: SourceEntry[]
    file_types?: FileType[]
    preserve_empty_dirs?: boolean
    filters?: BackupFilters
}

export type FileType =
    | 'file'
    | 'directory'
    | 'symlink'
    | 'fifo'
    | 'character_device'
    | 'block_device'
    | 'socket'
export type ContentFileType = Exclude<FileType, 'directory' | 'socket'>

export interface SourceEntry {
    path: string
    type: FileType
}

export interface SourcePlan {
    scope: {
        path: string
        selection?: SourceEntry[]
        file_types?: FileType[]
        preserve_empty_dirs?: boolean
        filters?: BackupFilters
    }
    items: (SourceEntry & { restore_path: string })[]
    merged_count: number
}

export type Compression = 'none' | 'huffman' | 'lz77'

export interface Target {
    id: string
    name: string
    host: string
    port: number
    mode?: 'local' | 'remote'
    ca_certificate?: string
    repository_path?: string
    data_root?: string
    compression?: Compression
    repository_encryption_id?: string
}

export interface RealtimeStatus {
    state:
        | 'WATCHING'
        | 'PENDING'
        | 'BACKING_UP'
        | 'RETRYING'
        | 'WAITING_CONFIRMATION'
        | 'WAITING_UNLOCK'
        | 'WAITING_CONNECTION'
        | 'WAITING_AUTH'
        | 'CERTIFICATE_FAILED'
        | 'DEGRADED'
    error: string
    monitor_error: string
    operation_id: string
}

export interface Configuration {
    realtime_status?: Record<string, RealtimeStatus>
    agent_busy?: boolean
    agent_pid?: number
    tasks: BackupTask[]
    targets: Target[]
}

export interface Warning {
    field?: string
    path: string
    reason: string
    type?: string
    severity?: 'error'
}

export interface ScanResult {
    root: string
    files: number
    directories: number
    bytes: number
    stored_bytes?: number | string
    symlinks?: number
    hardlinks?: number
    fifos?: number
    character_devices?: number
    block_devices?: number
    sockets?: number
    complete: boolean
    warning_count: number
    error_count?: number
    metadata_warning_count?: number
    sample: {
        path: string
        type?: string
        size?: number
        sha256?: string
        link_target?: string
        link_to?: string
        device_major?: string
        device_minor?: string
    }[]
}

export interface Version {
    id: string
    task_id: string
    source: string
    source_name?: string
    selection?: SourceEntry[]
    file_types?: FileType[]
    preserve_empty_dirs?: boolean
    filters?: BackupFilters
    completed_at: string
    files: string
    directories: string
    bytes: string
    stored_bytes?: string
    compression?: Compression
    symlinks?: string
    hardlinks?: string
    fifos?: string
    character_devices?: string
    block_devices?: string
    sockets?: string
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
    source_name?: string
    selection?: SourceEntry[]
    file_types?: FileType[]
    preserve_empty_dirs?: boolean
    filters?: BackupFilters
    destination?: string
    version_id?: string
    archive_path?: string
    target: Target
    path?: string
    files: number
    directories?: number
    symlinks?: number
    hardlinks?: number
    fifos?: number
    character_devices?: number
    block_devices?: number
    sockets?: number
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
    result?: ScanResult & {
        version?: Version
        destination?: string
        archive_path?: string
        archive_bytes?: string
    }
}

export interface VersionDeletion {
    version: Pick<Version, 'id'> & Partial<Version>
    target: Target
    repositoryPath: string
    retryOperationId?: string
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
    configureEncryption(
        command: 'create' | 'unlock' | 'lock',
        id: string,
        password?: string,
    ): Promise<void>
    loginRemote(
        id: string,
        account: string,
        password: string,
    ): Promise<{ expires_in: number }>
    saveTarget(target: Partial<Target>): Promise<Configuration>
    pingServer(targetId?: string): Promise<{
        connected: boolean
        authRequired?: boolean
        certificateFailed?: boolean
        storageAvailable?: boolean
        locked?: boolean
        server?: string
        error?: string
        dataRoot?: string
    }>
    listTasks(): Promise<BackupTask[]>
    addFolder(targetId?: string): Promise<BackupTask | null>
    chooseSources(kind: 'files' | 'folders'): Promise<string[] | null>
    previewSources(
        paths: string[],
        targetId: string,
        fileTypes: ContentFileType[],
        preserveEmptyDirs: boolean,
        filters?: BackupFilters,
    ): Promise<SourcePlan>
    createTask(
        paths: string[],
        targetId: string,
        name: string,
        fileTypes: ContentFileType[],
        preserveEmptyDirs: boolean,
        filters?: BackupFilters,
    ): Promise<BackupTask>
    setTaskRealtime(id: string, enabled: boolean): Promise<Configuration>
    updateTaskFilters(id: string, filters: BackupFilters): Promise<BackupTask>
    previewTaskFilters(
        id: string,
        filters: BackupFilters,
    ): Promise<{ operation_id: string }>
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
    deleteVersion(
        targetId: string,
        versionId: string,
        repositoryPath: string,
    ): Promise<{ operation_id: string }>
    retryVersionDeletion(id: string): Promise<{ operation_id: string }>
    restoreVersion(
        targetId: string,
        versionId: string,
        destination: string,
    ): Promise<{ operation_id: string } | null>
    exportArchive(
        targetId: string,
        versionId: string,
    ): Promise<{ operation_id: string } | null>
    importArchive(
        targetId: string,
        password?: string,
        allowPlain?: boolean,
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
