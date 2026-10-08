import { useCallback, useEffect, useRef, useState } from 'react'
import type {
    BackupTask,
    Configuration,
    ContentFileType,
    Operation,
    Target,
    Version,
} from './types'

const maxStatusFailures = 3

export function useBackupConsole() {
    const [config, setConfig] = useState<Configuration>({
        tasks: [],
        targets: [],
    })
    const [targetId, setTargetId] = useState('local')
    const targetRef = useRef(targetId)
    const [tab, setTab] = useState('tasks')
    const [connection, setConnection] = useState({
        state: 'checking',
        detail: '正在检查服务',
        root: '',
        error: '',
    })
    const [message, setMessage] = useState('')
    const [activeId, setActiveId] = useState<string | null>(null)
    const [starting, setStarting] = useState('')
    const [mutating, setMutating] = useState(false)
    const [detail, setDetail] = useState<Operation | null>(null)
    const [recordDetail, setRecordDetail] = useState<Operation | null>(null)
    const [scans, setScans] = useState<Record<string, Operation>>({})
    const [backups, setBackups] = useState<Record<string, Operation>>({})
    const [preparingScanId, setPreparingScanId] = useState<string | null>(null)
    const [preparingBackupId, setPreparingBackupId] = useState<string | null>(
        null,
    )
    const [preparingRestore, setPreparingRestore] = useState(false)
    const [versions, setVersions] = useState<Version[]>([])
    const [versionTotal, setVersionTotal] = useState(0)
    const [versionNextOffset, setVersionNextOffset] = useState<number | null>(
        null,
    )
    const [operationConnection, setOperationConnection] = useState({
        id: '',
        lost: false,
        error: '',
    })
    const [versionError, setVersionError] = useState('')
    const [versionsLoading, setVersionsLoading] = useState(false)
    const [versionTaskId, setVersionTaskId] = useState('')
    const [records, setRecords] = useState<Operation[]>([])
    const [recordTotal, setRecordTotal] = useState(0)
    const [recordNextOffset, setRecordNextOffset] = useState<number | null>(
        null,
    )
    const [recordsLoading, setRecordsLoading] = useState(false)
    const [targetDraft, setTargetDraft] = useState<Partial<Target>>({
        name: '',
        host: '127.0.0.1',
        port: 9000,
    })
    const [removeCandidate, setRemoveCandidate] = useState<BackupTask | null>(
        null,
    )
    const [restoreCandidate, setRestoreCandidate] = useState<Version | null>(
        null,
    )
    const [restoreDestination, setRestoreDestination] = useState('')
    const connectionRequest = useRef(0)
    const versionRequest = useRef(0)
    const recordsRequest = useRef(0)
    const recordDetailRequest = useRef(0)
    const busy = activeId !== null || !!starting || mutating
    const target = config.targets.find((item) => item.id === targetId)
    const tasks = config.tasks.filter((item) => item.target_id === targetId)
    const canBackup = connection.state === 'online'

    function selectTarget(id: string) {
        if (id === targetRef.current) {
            return
        }
        targetRef.current = id
        ++connectionRequest.current
        ++versionRequest.current
        setTargetId(id)
        setConnection({
            state: 'checking',
            detail: '正在检查服务',
            root: '',
            error: '',
        })
        setVersions([])
        setVersionTotal(0)
        setVersionNextOffset(null)
        setVersionError('')
        setVersionsLoading(false)
        setVersionTaskId('')
        setDetail(null)
        clearRecordDetail()
    }

    function navigate(next: string) {
        setTab(next)
        setMessage('')
        clearRecordDetail()
    }

    function clearRecordDetail() {
        ++recordDetailRequest.current
        setRecordDetail(null)
    }

    function closeDetail(operation: Operation) {
        if (tab === 'records') {
            clearRecordDetail()
            return
        }
        if (operation.action === 'backup' && operation.task_id) {
            clearBackup(operation.task_id)
        }
        setDetail((old) => (old?.id === operation.id ? null : old))
    }

    const refreshConnection = useCallback(async () => {
        const request = ++connectionRequest.current
        setConnection((old) => ({
            ...old,
            state: 'checking',
            detail: '正在检查服务',
            error: '',
        }))
        try {
            const result = await window.backup.pingServer(targetId)
            if (request !== connectionRequest.current) {
                return
            }
            setConnection({
                state: !result.connected
                    ? 'offline'
                    : result.storageAvailable
                      ? 'online'
                      : 'storage-unavailable',
                detail: !result.connected
                    ? '服务未连接'
                    : result.storageAvailable
                      ? '服务可用'
                      : '存储不可用',
                root: result.dataRoot || '',
                error: result.error || '',
            })
        } catch (error) {
            if (request === connectionRequest.current) {
                setConnection({
                    state: 'offline',
                    detail: '服务未连接',
                    root: '',
                    error: String(error),
                })
            }
        }
    }, [targetId])

    const loadRecords = useCallback(async (offset = 0) => {
        const request = ++recordsRequest.current
        setRecordsLoading(true)
        try {
            const result = await window.backup.listOperations(offset, true)
            if (request !== recordsRequest.current) {
                return
            }
            setRecords((old) =>
                offset ? [...old, ...result.operations] : result.operations,
            )
            setRecordTotal(result.execution_total)
            setRecordNextOffset(result.next_offset)
            return result.operations
        } catch (error) {
            setMessage(`读取执行记录失败：${String(error)}`)
        } finally {
            if (request === recordsRequest.current) {
                setRecordsLoading(false)
            }
        }
    }, [])

    const loadVersions = useCallback(
        async (offset = 0) => {
            const request = ++versionRequest.current
            setVersionsLoading(true)
            setVersionError('')
            try {
                const result = await window.backup.listVersions(
                    targetId,
                    offset,
                    versionTaskId,
                )
                if (
                    request !== versionRequest.current ||
                    targetId !== targetRef.current
                ) {
                    return
                }
                setVersions((old) =>
                    offset ? [...old, ...result.versions] : result.versions,
                )
                setVersionTotal(result.total)
                setVersionNextOffset(result.next_offset)
            } catch (error) {
                if (request === versionRequest.current) {
                    setVersionError(`查询版本失败：${String(error)}`)
                }
            } finally {
                if (request === versionRequest.current) {
                    setVersionsLoading(false)
                }
            }
        },
        [targetId, versionTaskId],
    )

    useEffect(() => {
        void window.backup
            .getConfig()
            .then(setConfig)
            .catch((error) => setMessage(String(error)))
        void loadRecords().then((items) => {
            const running = items?.find((item) => item.state === 'RUNNING')
            if (running) {
                if (running.target?.id) {
                    selectTarget(running.target.id)
                }
                setActiveId(running.id)
                setDetail(running)
                if (running.task_id) {
                    const update =
                        running.action === 'scan' ? setScans : setBackups
                    update((old) => ({ ...old, [running.task_id!]: running }))
                }
            }
        })
    }, [loadRecords])

    useEffect(() => {
        if (!activeId) {
            void refreshConnection()
        }
    }, [refreshConnection, activeId])

    useEffect(() => {
        if (
            tab === 'versions' &&
            !activeId &&
            ['online', 'storage-unavailable'].includes(connection.state)
        ) {
            void loadVersions()
        }
        if (tab === 'records') {
            void loadRecords()
        }
    }, [tab, activeId, connection.state, loadVersions, loadRecords])

    useEffect(() => {
        if (!activeId) {
            return
        }
        let cancelled = false
        let polling = false
        let failures = 0
        let timer: ReturnType<typeof setTimeout>
        const poll = async () => {
            if (polling || cancelled) {
                return
            }
            polling = true
            clearTimeout(timer)
            try {
                let operation = await window.backup.getOperation(activeId, true)
                if (!cancelled && operation.state !== 'RUNNING') {
                    operation = await window.backup.getOperation(activeId)
                }
                if (cancelled) {
                    return
                }
                failures = 0
                setOperationConnection({ id: activeId, lost: false, error: '' })
                setDetail(operation)
                // Polling refreshes an open record without selecting it.
                setRecordDetail((old) =>
                    old?.id === operation.id ? operation : old,
                )
                setRecords((old) =>
                    old.map((record) =>
                        record.id === operation.id
                            ? { ...record, ...operation }
                            : record,
                    ),
                )
                if (operation.action === 'scan' && operation.task_id) {
                    const taskId = operation.task_id
                    setScans((old) => ({ ...old, [taskId]: operation }))
                }
                if (operation.action === 'backup' && operation.task_id) {
                    const taskId = operation.task_id
                    setBackups((old) => ({ ...old, [taskId]: operation }))
                }
                if (operation.state === 'RUNNING') {
                    timer = setTimeout(poll, 400)
                    return
                }
                setActiveId(null)
                if (
                    operation.action === 'backup' &&
                    operation.state.startsWith('SUCCEEDED') &&
                    operation.task_id
                ) {
                    clearScan(operation.task_id)
                }
                void loadRecords()
            } catch (error) {
                if (!cancelled) {
                    ++failures
                    setOperationConnection({
                        id: activeId,
                        lost: failures >= maxStatusFailures,
                        error: String(error),
                    })
                    if (failures < maxStatusFailures) {
                        timer = setTimeout(poll, 2000)
                    }
                }
            } finally {
                polling = false
            }
        }
        void poll()
        const retry = () => {
            failures = 0
            void poll()
        }
        window.addEventListener('focus', retry)
        window.addEventListener('backup:retry-operation', retry)
        return () => {
            cancelled = true
            clearTimeout(timer)
            window.removeEventListener('focus', retry)
            window.removeEventListener('backup:retry-operation', retry)
        }
    }, [activeId, loadRecords])

    function clearScan(taskId: string) {
        setScans((old) => {
            const next = { ...old }
            delete next[taskId]
            return next
        })
    }

    async function scanTask(taskId: string) {
        const task = config.tasks.find((item) => item.id === taskId)
        clearScan(taskId)
        setPreparingScanId(taskId)
        await run(() => window.backup.scanTask(taskId), '正在准备扫描', {
            action: 'scan',
            task_id: taskId,
            source: task?.path,
            source_name: task?.name,
            selection: task?.selection,
            file_types: task?.file_types,
            preserve_empty_dirs: task?.preserve_empty_dirs,
        })
        setPreparingScanId(null)
    }

    function clearBackup(taskId: string) {
        setBackups((old) => {
            const next = { ...old }
            delete next[taskId]
            return next
        })
    }

    async function backupTask(taskId: string) {
        const task = config.tasks.find((item) => item.id === taskId)
        clearBackup(taskId)
        setPreparingBackupId(taskId)
        await run(() => window.backup.backupTask(taskId), '正在准备备份', {
            action: 'backup',
            task_id: taskId,
            source: task?.path,
            source_name: task?.name,
            selection: task?.selection,
            file_types: task?.file_types,
            preserve_empty_dirs: task?.preserve_empty_dirs,
        })
        setPreparingBackupId(null)
    }

    async function run(
        start: () => Promise<{ operation_id: string } | null>,
        label: string,
        context: Pick<
            Operation,
            | 'action'
            | 'task_id'
            | 'source'
            | 'destination'
            | 'selection'
            | 'file_types'
            | 'preserve_empty_dirs'
            | 'source_name'
        >,
    ) {
        setMessage('')
        setStarting(label)
        try {
            const result = await start()
            if (result) {
                // The start receipt exists even when the first status query fails.
                const pending: Operation = {
                    ...context,
                    id: result.operation_id,
                    state: 'RUNNING',
                    stage: 'prepare',
                    started_at: new Date().toISOString(),
                    target: target!,
                    files: 0,
                    bytes: 0,
                    warning_count: 0,
                }
                setDetail(pending)
                if (pending.task_id) {
                    const update =
                        pending.action === 'scan' ? setScans : setBackups
                    update((old) => ({ ...old, [pending.task_id!]: pending }))
                }
                setOperationConnection({
                    id: result.operation_id,
                    lost: false,
                    error: '',
                })
                setActiveId(result.operation_id)
            }
        } catch (error) {
            setMessage(String(error))
        } finally {
            setStarting('')
        }
    }

    async function createTask(
        paths: string[],
        destinationId: string,
        name: string,
        fileTypes: ContentFileType[],
        preserveEmptyDirs: boolean,
    ) {
        setMutating(true)
        setMessage('')
        try {
            const created = await window.backup.createTask(
                paths,
                destinationId,
                name,
                fileTypes,
                preserveEmptyDirs,
            )
            const existing = config.tasks.some((item) => item.id === created.id)
            setConfig(await window.backup.getConfig())
            if (targetId !== destinationId) {
                selectTarget(destinationId)
            }
            setMessage(
                existing
                    ? '相同备份范围的任务已存在，已保留原任务。'
                    : '备份任务已创建',
            )
        } finally {
            setMutating(false)
        }
    }

    async function removeTask() {
        if (!removeCandidate) {
            return
        }
        const task = removeCandidate
        setRemoveCandidate(null)
        setMutating(true)
        try {
            await window.backup.removeTask(task.id)
            setConfig(await window.backup.getConfig())
            clearScan(task.id)
            clearBackup(task.id)
            if (detail?.task_id === task.id) {
                setDetail(null)
            }
            if (versionTaskId === task.id) {
                setVersionTaskId('')
            }
        } catch (error) {
            setMessage(String(error))
        } finally {
            setMutating(false)
        }
    }

    async function saveTarget(event: React.FormEvent) {
        event.preventDefault()
        setMutating(true)
        try {
            const next = await window.backup.saveTarget(targetDraft)
            setConfig(next)
            setTargetDraft({ name: '', host: '127.0.0.1', port: 9000 })
            ++connectionRequest.current
            ++versionRequest.current
            setVersions([])
            setVersionTotal(0)
            setVersionNextOffset(null)
            setVersionsLoading(false)
            setMessage(
                activeId
                    ? '配置已保存，将用于下次执行；当前操作继续使用原配置。'
                    : '目标已保存',
            )
            if (activeId) {
                setConnection({
                    state: 'unchecked',
                    detail: '目标配置已更新，待检查',
                    root: '',
                    error: '',
                })
            } else {
                void refreshConnection()
            }
        } catch (error) {
            setMessage(String(error))
        } finally {
            setMutating(false)
        }
    }

    async function showRecord(id: string) {
        const request = ++recordDetailRequest.current
        setRecordDetail(null)
        try {
            const operation = await window.backup.getOperation(id)
            if (request === recordDetailRequest.current) {
                setRecordDetail(operation)
            }
        } catch (error) {
            if (request === recordDetailRequest.current) {
                setMessage(String(error))
            }
        }
    }

    function viewVersions(task?: BackupTask) {
        if (task) {
            selectTarget(task.target_id)
        }
        setVersionTaskId(task?.id || '')
        setVersions([])
        setVersionTotal(0)
        setVersionNextOffset(null)
        navigate('versions')
    }

    async function chooseRestoreDirectory() {
        setMutating(true)
        try {
            const directory = await window.backup.chooseRestoreDirectory()
            if (directory) {
                setRestoreDestination(directory)
            }
        } catch (error) {
            setMessage(String(error))
        } finally {
            setMutating(false)
        }
    }

    function requestRestore(version: Version) {
        setMessage('')
        setRestoreDestination('')
        setRestoreCandidate(version)
    }

    async function restore() {
        if (!restoreCandidate || !restoreDestination) {
            return
        }
        const version = restoreCandidate
        setRestoreCandidate(null)
        setDetail(null)
        setPreparingRestore(true)
        await run(
            () =>
                window.backup.restoreVersion(
                    targetId,
                    version.id,
                    restoreDestination,
                ),
            '正在准备还原',
            { action: 'restore', destination: restoreDestination },
        )
        setPreparingRestore(false)
    }

    async function confirmOperation(operation: Operation) {
        setMutating(true)
        try {
            const confirmed = await window.backup.confirmOperation(
                operation.id,
                operation.target.id,
            )
            setDetail((old) => (old?.id === confirmed.id ? confirmed : old))
            setRecordDetail((old) =>
                old?.id === confirmed.id ? confirmed : old,
            )
            const taskId = confirmed.task_id
            if (confirmed.action === 'backup' && taskId) {
                setBackups((old) =>
                    old[taskId]?.id === confirmed.id
                        ? { ...old, [taskId]: confirmed }
                        : old,
                )
                if (confirmed.state.startsWith('SUCCEEDED')) {
                    setScans((old) => {
                        if (
                            !old[taskId] ||
                            old[taskId].started_at > confirmed.started_at
                        ) {
                            return old
                        }
                        const next = { ...old }
                        delete next[taskId]
                        return next
                    })
                }
            }
            await loadRecords()
            if (tab === 'versions') {
                void loadVersions()
            }
        } catch (error) {
            setMessage(String(error))
        } finally {
            setMutating(false)
        }
    }

    return {
        config,
        tasks,
        selectTarget,
        targetId,
        tab,
        navigate,
        connection,
        message,
        setMessage,
        detail: tab === 'records' ? recordDetail : detail,
        closeDetail,
        scans,
        scanTask,
        preparingScanId,
        backups,
        backupTask,
        preparingBackupId,
        preparingRestore,
        versions,
        versionTotal,
        versionNextOffset,
        versionError,
        versionsLoading,
        versionTaskId,
        setVersionTaskId,
        records,
        recordTotal,
        recordNextOffset,
        recordsLoading,
        targetDraft,
        setTargetDraft,
        busy,
        mutating,
        canBackup,
        operationConnection,
        retryOperation: () =>
            window.dispatchEvent(new Event('backup:retry-operation')),
        starting,
        target,
        refreshConnection,
        loadRecords,
        loadVersions,
        createTask,
        removeTask,
        saveTarget,
        showRecord,
        viewVersions,
        removeCandidate,
        setRemoveCandidate,
        restoreCandidate,
        setRestoreCandidate,
        restoreDestination,
        chooseRestoreDirectory,
        requestRestore,
        restore,
        confirmOperation,
    }
}

export type ConsoleModel = ReturnType<typeof useBackupConsole>
