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
import { ScanTreeTable } from './scan-tree-table'

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
    filter_preview: '筛选预览',
    backup: '备份',
    restore: '还原',
    delete_version: '删除版本',
    export_archive: '导出归档',
    import_archive: '导入归档',
}

export const compressionLabels = {
    none: '不压缩',
    huffman: 'Huffman',
    lz77: 'LZ77',
}
export const stageLabels: Record<string, string> = {
    prepare: '准备',
    scan: '扫描',
    upload: '上传',
    verify: '验证清单',
    restore: '还原',
    commit: '保存版本',
    confirm: '确认结果',
    delete: '删除版本',
    archive_verify: '校验归档',
    archive_export: '打包归档',
    archive_import: '导入归档',
}

export function archiveError(error: string): string {
    if (error.includes('Authentication failed')) {
        return '归档密码错误，或密文已损坏。'
    }
    if (error.includes('ENCRYPTED_IMPORT_CONFIRM')) {
        return '请使用“导入加密归档”并确认允许保存未加密内容，或选择加密仓库。'
    }
    if (error.includes('REPOSITORY_LOCKED')) {
        return '请先到备份目标解锁仓库。'
    }
    if (error.includes('Password must contain')) {
        return '请使用“导入加密归档”输入密码。'
    }
    if (error.includes('Archive destination already exists')) {
        return '所选归档文件已存在，请更换文件名。'
    }
    if (/Unsupported archive|Encrypted archives/.test(error)) {
        return `不支持此归档格式，请选择本软件导出的归档。${error}`
    }
    if (
        /checksum|Truncated|Incomplete archive|archive.*mismatch/i.test(error)
    ) {
        return `归档校验失败，文件可能损坏或不完整。${error}`
    }
    if (/No space left/i.test(error)) {
        return `存储空间不足，请清理空间后重试。${error}`
    }
    return error
}

export function deletionError(error: string): string {
    if (error.includes('Version is in use')) {
        return '该版本正在被读取或还原，请等待完成后重试删除。'
    }
    if (
        error.includes('DELETE_UNCERTAIN') ||
        error.includes('Deletion may be incomplete')
    ) {
        return `删除结果尚未确认，可重试删除以继续处理。${error}`
    }
    return error
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
                <ScanTreeTable
                    key={`tree-${operationId}`}
                    result={result}
                    typeLabels={entryTypeLabels}
                    formatSize={formatBytes}
                />
            )}
        </section>
    )
}
