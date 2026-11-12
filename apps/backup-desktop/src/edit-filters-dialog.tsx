import { useEffect, useRef, useState } from 'react'
import type { BackupTask, ScanResult } from './types'
import type { ConsoleModel } from './use-backup-console'
import { FilterEditor, emptyFilters, filterError } from './backup-filters'
import { GlassButton } from './glass-controls'
import { Dialog } from './workflow'
import { formatBytes } from './ui'

export function EditFiltersDialog({
    task,
    model,
    onClose,
}: {
    task: BackupTask
    model: ConsoleModel
    onClose: () => void
}) {
    const [filters, setFilters] = useState(task.filters || emptyFilters())
    const [pending, setPending] = useState(false)
    const [error, setError] = useState('')
    const [result, setResult] = useState<ScanResult | null>(null)
    const alive = useRef(true)
    useEffect(() => {
        alive.current = true
        return () => {
            alive.current = false
        }
    }, [])
    async function preview() {
        setPending(true)
        setError('')
        setResult(null)
        try {
            const started = await window.backup.previewTaskFilters(
                task.id,
                filters,
            )
            while (alive.current) {
                const operation = await window.backup.getOperation(
                    started.operation_id,
                )
                if (!alive.current) {
                    return
                }
                if (operation.state !== 'RUNNING') {
                    if (
                        !operation.state.startsWith('SUCCEEDED') ||
                        !operation.result?.complete
                    ) {
                        throw new Error(
                            operation.error ||
                                '预览未完成，请检查执行记录中的问题。',
                        )
                    }
                    setResult(operation.result)
                    return
                }
                await new Promise((resolve) => setTimeout(resolve, 250))
            }
        } catch (failure) {
            if (alive.current) {
                setError(String(failure))
            }
        } finally {
            if (alive.current) {
                setPending(false)
            }
        }
    }
    async function save() {
        setError('')
        try {
            await model.updateTaskFilters(task.id, filters)
            onClose()
        } catch (failure) {
            setError(String(failure))
        }
    }
    return (
        <Dialog
            title="编辑自定义备份"
            onClose={() => {
                if (!pending && !model.mutating) {
                    onClose()
                }
            }}
        >
            <div className="filter-dialog-body">
                <p className="path">{task.name || task.path}</p>
                <p className="muted">
                    预览使用当前草稿，保存后才更改任务。已有备份版本保留原规则。
                </p>
                <FilterEditor
                    value={filters}
                    disabled={pending || model.busy}
                    onChange={(next) => {
                        setFilters(next)
                        setResult(null)
                        setError('')
                    }}
                />
                {result && (
                    <div className="filter-preview" role="status">
                        <p>
                            {result.files} 个普通文件 ·{' '}
                            {formatBytes(result.bytes)} · {result.directories}{' '}
                            个目录
                        </p>
                        <p>
                            软链接 {result.symlinks || 0} · 硬链接{' '}
                            {result.hardlinks || 0} · 管道 {result.fifos || 0} ·
                            警告 {result.warning_count || 0}
                        </p>
                        <ul>
                            {result.sample.map((entry) => (
                                <li key={entry.path} className="path">
                                    {entry.path}
                                </li>
                            ))}
                        </ul>
                        <small>
                            预览最多显示 100
                            项，统计覆盖完整入选范围；有警告时可在执行记录查看。
                        </small>
                    </div>
                )}
                {error && (
                    <p className="error-text" role="alert">
                        {error}
                    </p>
                )}
            </div>
            <div className="dialog-actions">
                <GlassButton
                    disabled={pending || model.mutating}
                    onClick={onClose}
                >
                    取消
                </GlassButton>
                <GlassButton
                    className="preview-filters"
                    disabled={pending || model.busy || !!filterError(filters)}
                    onClick={() => void preview()}
                >
                    {pending ? '正在预览' : '预览范围'}
                </GlassButton>
                <GlassButton
                    className="backup-button save-filters"
                    disabled={pending || model.busy || !!filterError(filters)}
                    onClick={() => void save()}
                >
                    保存规则
                </GlassButton>
            </div>
        </Dialog>
    )
}
