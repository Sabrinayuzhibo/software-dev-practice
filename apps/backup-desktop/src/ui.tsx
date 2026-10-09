import { GlassButton } from './glass-controls'
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

export const entryTypeLabels: Record<string, string> = {
    file: '文件',
    directory: '目录',
    symlink: '软链接',
    hardlink: '硬链接',
    fifo: '命名管道',
    character_device: '字符设备',
    block_device: '块设备',
    socket: '套接字',
    special: '特殊条目',
    unknown: '未知类型',
}

export function specialEntryCounts(value: {
    symlinks?: number | string
    hardlinks?: number | string
    fifos?: number | string
    character_devices?: number | string
    block_devices?: number | string
    sockets?: number | string
}): string {
    return [
        Number(value.symlinks) ? `${value.symlinks} 个软链接` : '',
        Number(value.hardlinks) ? `${value.hardlinks} 个硬链接` : '',
        Number(value.fifos) ? `${value.fifos} 个命名管道` : '',
        Number(value.character_devices)
            ? `${value.character_devices} 个字符设备`
            : '',
        Number(value.block_devices) ? `${value.block_devices} 个块设备` : '',
        Number(value.sockets) ? `${value.sockets} 个套接字` : '',
    ]
        .filter(Boolean)
        .join(' · ')
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
                <GlassButton
                    className="window-control"
                    title="最小化"
                    aria-label="最小化"
                    onClick={() => void window.backup.minimizeWindow()}
                >
                    <Minus />
                </GlassButton>
                <GlassButton
                    className="window-control"
                    title={maximized ? '还原' : '最大化'}
                    aria-label={maximized ? '还原' : '最大化'}
                    onClick={() => void window.backup.toggleMaximizeWindow()}
                >
                    {maximized ? <Maximize /> : <Square />}
                </GlassButton>
                <GlassButton
                    className="window-control close"
                    title="关闭"
                    aria-label="关闭"
                    onClick={() => void window.backup.closeWindow()}
                >
                    <X />
                </GlassButton>
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
                <GlassButton
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
                </GlassButton>
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
            {specialEntryCounts(result) && (
                <p className="muted special-counts">
                    {specialEntryCounts(result)}
                </p>
            )}
            {result.complete &&
                result.files === 0 &&
                !result.symlinks &&
                !result.fifos &&
                !result.character_devices &&
                !result.block_devices &&
                !result.sockets && (
                    <p className="muted scan-empty">本次没有匹配文件</p>
                )}
            {!!result.warning_count && (
                <WarningDetails
                    key={operationId}
                    operationId={operationId}
                    count={result.warning_count}
                    className="scan-warnings"
                    label={result.complete ? '项警告' : '项问题'}
                />
            )}
            {expanded && (
                <>
                    <div className="table-scroll">
                        <table className="scan-table">
                            <thead>
                                <tr>
                                    <th>路径</th>
                                    <th>类型</th>
                                    <th>大小</th>
                                    <th>内容校验值</th>
                                </tr>
                            </thead>
                            <tbody>
                                {result.sample.map((file) => (
                                    <tr
                                        key={file.path}
                                        data-type={file.type || 'file'}
                                    >
                                        <td>
                                            {file.path}
                                            {file.link_target !== undefined && (
                                                <small>
                                                    指向：{file.link_target}
                                                </small>
                                            )}
                                            {file.link_to && (
                                                <small>
                                                    同组文件：{file.link_to}
                                                </small>
                                            )}
                                            {file.device_major !== undefined &&
                                                file.device_minor !==
                                                    undefined && (
                                                    <small>
                                                        设备号：
                                                        {file.device_major}:
                                                        {file.device_minor}
                                                    </small>
                                                )}
                                        </td>
                                        <td>
                                            {entryTypeLabels[
                                                file.type || 'file'
                                            ] || '未知类型'}
                                        </td>
                                        <td>
                                            {file.size === undefined
                                                ? '-'
                                                : formatBytes(file.size)}
                                        </td>
                                        <td className="digest">
                                            {file.sha256 ? (
                                                <>
                                                    <span
                                                        className="digest-short"
                                                        title={`SHA-256: ${file.sha256}`}
                                                    >
                                                        {file.sha256}
                                                    </span>
                                                    <details className="digest-details">
                                                        <summary>
                                                            SHA-256
                                                        </summary>
                                                        <code>
                                                            {file.sha256}
                                                        </code>
                                                    </details>
                                                </>
                                            ) : (
                                                '-'
                                            )}
                                        </td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                    </div>
                    <p className="muted">显示 {result.sample.length} 个条目</p>
                </>
            )}
        </section>
    )
}
