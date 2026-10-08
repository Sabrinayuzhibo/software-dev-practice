import {
    FilePlus,
    FolderPlus,
    File,
    Folder,
    X,
    LoaderCircle,
    Plus,
} from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import type { ContentFileType, SourcePlan } from './types'
import type { ConsoleModel } from './use-backup-console'
import { Dialog } from './workflow'
import { taskTitle } from './source-scope'
import { entryTypeLabels } from './ui'
import { FileTypePicker, defaultFileTypes } from './file-type-filter'

function sourceError(failure: unknown): string {
    const message = String(failure)
        .replace(/^Error:\s*/, '')
        .replace(/^Error invoking remote method '[^']+': Error:\s*/, '')
    const translations: Record<string, string> = {
        'Cannot inspect source: ': '无法访问来源，请检查路径和权限：',
        'Cannot resolve path: ': '无法解析来源路径：',
        'Unsupported source type: ': '不支持将此类型作为备份来源：',
        'Source overlaps Agent state': '所选来源包含应用数据，请重新选择。',
        'Source overlaps repository': '所选来源与备份仓库重叠，请重新选择。',
        'Source selection exceeds supported size':
            '所选路径过多或过长，请拆分为多个任务。',
        'Task name must be at most 120 characters': '任务名称最多 120 个字符。',
        'Source already configured for this repository under another target':
            '同一仓库的其他目标下已存在相同范围的任务。',
        'Selected source does not match file type filter: ':
            '所选文件的类型不在当前筛选范围内：',
    }
    for (const [prefix, translated] of Object.entries(translations)) {
        if (message.startsWith(prefix)) {
            return translated + message.slice(prefix.length)
        }
    }
    return message
}

function SourceList({
    paths,
    plan,
    disabled,
    onRemove,
}: {
    paths: string[]
    plan: SourcePlan | null
    disabled: boolean
    onRemove: (path: string) => void
}) {
    if (!paths.length) {
        return <p className="source-empty">尚未选择文件或文件夹</p>
    }
    return (
        <div className="source-list table-scroll">
            <table>
                <thead>
                    <tr>
                        <th>备份来源</th>
                        <th>还原路径</th>
                        <th aria-label="移除来源" />
                    </tr>
                </thead>
                <tbody>
                    {paths.map((path) => {
                        const item = plan?.items.find(
                            (item) => item.path === path,
                        )
                        return (
                            <tr key={path} data-source-path={path}>
                                <td>
                                    <span className="source-item-name">
                                        {item?.type === 'directory' ? (
                                            <Folder size={16} />
                                        ) : (
                                            <File size={16} />
                                        )}
                                        {path.split('/').pop() || path}
                                    </span>
                                    <small className="path">{path}</small>
                                    {item && (
                                        <small>
                                            {entryTypeLabels[item.type]}
                                        </small>
                                    )}
                                </td>
                                <td className="path">
                                    {item?.restore_path === '.'
                                        ? '目录内原有结构'
                                        : item?.restore_path || '待检查'}
                                </td>
                                <td>
                                    <button
                                        className="icon-button remove-source"
                                        title="移除来源"
                                        aria-label={`移除来源 ${path}`}
                                        disabled={disabled}
                                        onClick={() => onRemove(path)}
                                    >
                                        <X size={16} />
                                    </button>
                                </td>
                            </tr>
                        )
                    })}
                </tbody>
            </table>
        </div>
    )
}

export function CreateTaskDialog({
    model,
    onClose,
}: {
    model: ConsoleModel
    onClose: () => void
}) {
    const [paths, setPaths] = useState<string[]>([])
    const [plan, setPlan] = useState<SourcePlan | null>(null)
    const [name, setName] = useState('')
    const [targetId, setTargetId] = useState(model.targetId)
    const [fileTypes, setFileTypes] =
        useState<ContentFileType[]>(defaultFileTypes)
    const [manualPath, setManualPath] = useState('')
    const [showManualPath, setShowManualPath] = useState(false)
    const [preserveEmptyDirs, setPreserveEmptyDirs] = useState(true)
    const [pending, setPending] = useState(false)
    const [error, setError] = useState('')
    const request = useRef(0)
    useEffect(
        () => () => {
            ++request.current
        },
        [],
    )

    const close = () => {
        if (!model.mutating) {
            ++request.current
            onClose()
        }
    }
    async function preview(
        next: string[],
        destinationId = targetId,
        types = fileTypes,
        preserve = preserveEmptyDirs,
    ) {
        const current = ++request.current
        setPaths([...new Set(next)])
        setPlan(null)
        setError('')
        if (!types.length) {
            setPending(false)
            setError('至少选择一种可备份类型。')
            return
        }
        if (!next.length) {
            setPending(false)
            return
        }
        setPending(true)
        try {
            const result = await window.backup.previewSources(
                next,
                destinationId,
                types,
                preserve,
            )
            if (current === request.current) {
                setPlan(result)
                setPaths(result.items.map((item) => item.path))
            }
        } catch (failure) {
            if (current === request.current) {
                setError(sourceError(failure))
            }
        } finally {
            if (current === request.current) {
                setPending(false)
            }
        }
    }
    async function choose(kind: 'files' | 'folders') {
        const current = ++request.current
        setPending(true)
        try {
            const selected = await window.backup.chooseSources(kind)
            if (current !== request.current) {
                return
            }
            setPending(false)
            if (selected?.length) {
                await preview([...paths, ...selected])
            }
        } catch (failure) {
            if (current === request.current) {
                setError(sourceError(failure))
                setPending(false)
            }
        }
    }
    async function save() {
        if (!plan || pending || model.busy || !fileTypes.length) {
            return
        }
        setError('')
        try {
            await model.createTask(
                paths,
                targetId,
                name.trim(),
                fileTypes,
                preserveEmptyDirs,
            )
            onClose()
        } catch (failure) {
            setError(sourceError(failure))
        }
    }
    const disabled = pending || model.busy
    return (
        <Dialog title="新建备份任务" onClose={close}>
            <form
                id="create-task-form"
                className="create-task-form"
                onSubmit={(event) => {
                    event.preventDefault()
                    void save()
                }}
            >
                <label>
                    任务名称
                    <input
                        aria-label="任务名称"
                        maxLength={120}
                        value={name}
                        placeholder={
                            plan
                                ? taskTitle(plan.scope)
                                : '可选，默认使用来源名称'
                        }
                        disabled={model.mutating}
                        onChange={(event) => setName(event.target.value)}
                    />
                </label>
                <label>
                    备份目标
                    <select
                        aria-label="新任务备份目标"
                        value={targetId}
                        disabled={disabled}
                        onChange={(event) => {
                            setTargetId(event.target.value)
                            void preview(paths, event.target.value)
                        }}
                    >
                        {model.config.targets.map((target) => (
                            <option key={target.id} value={target.id}>
                                {target.name}
                            </option>
                        ))}
                    </select>
                </label>
            </form>
            <FileTypePicker
                selected={fileTypes}
                preserveEmptyDirs={preserveEmptyDirs}
                disabled={disabled}
                onChange={(types) => {
                    setFileTypes(types)
                    void preview(paths, targetId, types)
                }}
                onEmptyDirectoryChange={(preserve) => {
                    setPreserveEmptyDirs(preserve)
                    void preview(paths, targetId, fileTypes, preserve)
                }}
            />
            <div className="source-actions">
                <button
                    className="choose-files"
                    disabled={disabled}
                    onClick={() => void choose('files')}
                >
                    <FilePlus size={16} />
                    添加文件
                </button>
                <button
                    className="choose-folders"
                    disabled={disabled}
                    onClick={() => void choose('folders')}
                >
                    <FolderPlus size={16} />
                    添加文件夹
                </button>
                {fileTypes.some((type) => !defaultFileTypes.includes(type)) && (
                    <button
                        className="choose-special-path"
                        disabled={disabled}
                        onClick={() => setShowManualPath(!showManualPath)}
                    >
                        <Plus size={16} />
                        添加特殊路径
                    </button>
                )}
                <span className="muted" role="status">
                    {pending ? '正在检查来源' : `已选 ${paths.length} 项`}
                </span>
            </div>
            {showManualPath && (
                <div className="source-path-input">
                    <input
                        aria-label="特殊节点绝对路径"
                        placeholder="设备或套接字绝对路径"
                        value={manualPath}
                        disabled={disabled}
                        onChange={(event) => setManualPath(event.target.value)}
                        onKeyDown={(event) => {
                            if (event.key === 'Enter' && manualPath.trim()) {
                                void preview([...paths, manualPath.trim()])
                                setManualPath('')
                            }
                        }}
                    />
                    <button
                        className="icon-button"
                        aria-label="添加路径"
                        title="添加路径"
                        disabled={disabled || !manualPath.trim()}
                        onClick={() => {
                            void preview([...paths, manualPath.trim()])
                            setManualPath('')
                        }}
                    >
                        <Plus size={16} />
                    </button>
                </div>
            )}
            <SourceList
                paths={paths}
                plan={plan}
                disabled={disabled}
                onRemove={(path) =>
                    void preview(paths.filter((value) => value !== path))
                }
            />
            {!!plan?.merged_count && (
                <p className="muted" role="status">
                    已合并 {plan.merged_count} 项重复或被文件夹包含的来源
                </p>
            )}
            {error && (
                <p className="error-text" role="alert">
                    {error}
                </p>
            )}
            {error && paths.length > 0 && (
                <button
                    className="source-recheck"
                    disabled={disabled}
                    onClick={() => void preview(paths)}
                >
                    重新检查
                </button>
            )}
            <div className="dialog-actions">
                <button disabled={model.mutating} onClick={close}>
                    取消
                </button>
                <button
                    type="submit"
                    form="create-task-form"
                    className="backup-button create-task"
                    disabled={disabled || !plan || !fileTypes.length}
                >
                    {model.mutating ? (
                        <LoaderCircle size={16} className="spinning" />
                    ) : (
                        <Plus size={16} />
                    )}
                    {model.mutating ? '正在创建' : '创建任务'}
                </button>
            </div>
        </Dialog>
    )
}
