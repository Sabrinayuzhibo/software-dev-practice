import { useCallback, useEffect, useRef, useState } from 'react'
import type {
    BackupTask,
    Configuration,
    Operation,
    Target,
    Version,
} from './types'

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
                state: result.connected ? 'online' : 'offline',
                detail: result.connected ? '服务可用' : '服务未连接',
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
                setActiveId(running.id)
                setDetail(running)
            }
        })
    }, [loadRecords])

    useEffect(() => {
        if (!activeId) {
            void refreshConnection()
        }
    }, [refreshConnection, activeId])

    useEffect(() => {
        if (tab === 'versions' && !activeId && connection.state === 'online') {
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
                    setMessage(`执行状态暂时不可用，正在重试：${String(error)}`)
                    timer = setTimeout(poll, 2000)
                }
            } finally {
                polling = false
            }
        }
        void poll()
        window.addEventListener('focus', poll)
        return () => {
            cancelled = true
            clearTimeout(timer)
            window.removeEventListener('focus', poll)
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
        clearScan(taskId)
        setPreparingScanId(taskId)
        await run(() => window.backup.scanTask(taskId), '正在准备扫描')
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
        clearBackup(taskId)
        setPreparingBackupId(taskId)
        await run(() => window.backup.backupTask(taskId), '正在准备备份')
        setPreparingBackupId(null)
    }

    async function run(
        start: () => Promise<{ operation_id: string } | null>,
        label = '正在准备',
    ) {
        setMessage('')
        setStarting(label)
        try {
            const result = await start()
            if (result) {
                setDetail(null)
                setActiveId(result.operation_id)
            }
        } catch (error) {
            setMessage(String(error))
        } finally {
            setStarting('')
        }
    }

    async function addFolder() {
        setMutating(true)
        setMessage('')
        try {
            await window.backup.addFolder(targetId)
            setConfig(await window.backup.getConfig())
        } catch (error) {
            setMessage(String(error))
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
            setMessage('目标已保存')
            void refreshConnection()
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
        starting,
        target,
        refreshConnection,
        loadRecords,
        loadVersions,
        addFolder,
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
