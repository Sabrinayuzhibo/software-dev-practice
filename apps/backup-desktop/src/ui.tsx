import {
    Archive,
    ChevronDown,
    ChevronUp,
    Maximize,
    Minus,
    Square,
    X,
} from 'lucide-react'
import { useEffect, useState } from 'react'
import type { ScanResult } from './types'
import { WarningDetails } from './warning-details'

export const stateLabels: Record<string, string> = {
    RUNNING: '执行中',
    WAITING: '结果待确认',
    SUCCEEDED: '成功',
    SUCCEEDED_WITH_WARNINGS: '完成并有警告',
    FAILED: '失败',
    INTERRUPTED: '已中断',
}
export const actionLabels: Record<string, string> = {
    scan: '扫描',
    backup: '备份',
    restore: '还原',
}
export const stageLabels: Record<string, string> = {
    prepare: '准备',
    scan: '扫描',
    upload: '上传',
    verify: '验证清单',
    restore: '还原',
    commit: '保存版本',
    confirm: '确认结果',
}

export function formatBytes(value: number | string): string {
    const bytes = Number(value)
    if (bytes < 1024) {
        return `${bytes} B`
    }
    if (bytes < 1024 ** 2) {
        return `${(bytes / 1024).toFixed(1)} KiB`
    }
    if (bytes < 1024 ** 3) {
        return `${(bytes / 1024 ** 2).toFixed(1)} MiB`
    }
    return `${(bytes / 1024 ** 3).toFixed(2)} GiB`
}

export function formatTime(value: string): string {
    return new Date(value).toLocaleString('zh-CN', { hour12: false })
}

export function TitleBar() {
    const [maximized, setMaximized] = useState(false)
    useEffect(() => {
        const unsubscribe = window.backup.onWindowMaximizedChange(setMaximized)
        void window.backup.isWindowMaximized().then(setMaximized)
        return unsubscribe
    }, [])
    return (
        <div className="window-titlebar">
            <div className="titlebar-brand">
                <Archive size={18} /> Backup System
            </div>
            <div
                className="titlebar-context"
                onDoubleClick={() => void window.backup.toggleMaximizeWindow()}
            >
                本地备份
            </div>
            <div className="window-controls" role="group" aria-label="窗口控制">
                <button
                    className="window-control"
                    title="最小化"
                    aria-label="最小化"
                    onClick={() => void window.backup.minimizeWindow()}
                >
                    <Minus />
                </button>
                <button
                    className="window-control"
                    title={maximized ? '还原' : '最大化'}
                    aria-label={maximized ? '还原' : '最大化'}
                    onClick={() => void window.backup.toggleMaximizeWindow()}
                >
                    {maximized ? <Maximize /> : <Square />}
                </button>
                <button
                    className="window-control close"
                    title="关闭"
                    aria-label="关闭"
                    onClick={() => void window.backup.closeWindow()}
                >
                    <X />
                </button>
            </div>
        </div>
    )
}

export function ScanPreview({
    operationId,
    result,
    finishedAt,
}: {
    operationId: string
    result: ScanResult
    finishedAt?: string
}) {
    const [expanded, setExpanded] = useState(true)
    return (
        <section className="scan-section">
            <div className="section-heading scan-heading">
                <div className="inline-actions">
                    <h3>扫描预览</h3>
                    {finishedAt && (
                        <time className="muted" dateTime={finishedAt}>
                            {formatTime(finishedAt)}
                        </time>
                    )}
                </div>
                <button
                    className="icon-button preview-toggle"
                    aria-label={
                        expanded ? '收起扫描文件明细' : '展开扫描文件明细'
                    }
                    title={expanded ? '收起文件明细' : '展开文件明细'}
                    aria-expanded={expanded}
                    onClick={() => setExpanded(!expanded)}
                >
                    {expanded ? (
                        <ChevronUp size={18} />
                    ) : (
                        <ChevronDown size={18} />
                    )}
                </button>
            </div>
            <div className="metrics">
                <span>
                    文件 <strong>{result.files}</strong>
                </span>
                <span>
                    目录 <strong>{result.directories}</strong>
                </span>
                <span>
                    总大小 <strong>{formatBytes(result.bytes)}</strong>
                </span>
                <span>
                    状态{' '}
                    <strong>
                        {!result.complete
                            ? '不完整'
                            : result.warning_count
                              ? '完成并有警告'
                              : '扫描完成'}
                    </strong>
                </span>
            </div>
            {!!result.warning_count && (
                <WarningDetails
                    key={operationId}
                    operationId={operationId}
                    count={result.warning_count}
                    className="scan-warnings"
                />
            )}
            {expanded && (
                <>
                    <div className="table-scroll">
                        <table className="scan-table">
                            <thead>
                                <tr>
                                    <th>路径</th>
                                    <th>大小</th>
                                    <th>内容校验值</th>
                                </tr>
                            </thead>
                            <tbody>
                                {result.sample.map((file) => (
                                    <tr key={file.path}>
                                        <td>{file.path}</td>
                                        <td>{formatBytes(file.size)}</td>
                                        <td className="digest">
                                            <span
                                                className="digest-short"
                                                title={`SHA-256: ${file.sha256}`}
                                            >
                                                {file.sha256}
                                            </span>
                                            <details className="digest-details">
                                                <summary>SHA-256</summary>
                                                <code>{file.sha256}</code>
                                            </details>
                                        </td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                    </div>
                    <p className="muted">
                        显示前 {result.sample.length} 个文件
                    </p>
                </>
            )}
        </section>
    )
}
