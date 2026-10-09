import { GlassButton } from './glass-controls'
import {
    Archive,
    File,
    Files,
    Folder,
    RefreshCw,
    ScanLine,
    Trash2,
    Upload,
} from 'lucide-react'
import type { BackupTask } from './types'
import type { ConsoleModel } from './use-backup-console'
import { ScanPreview, formatBytes, formatTime, stateLabels } from './ui'
import { OperationPanel } from './workflow'
import { OperationConnection } from './operation-connection'
import { SourceDetails, sourceLabel, taskTitle } from './source-scope'
import { entryTypeLabels } from './ui'
import { FileTypeSummary } from './file-type-filter'

export function TaskCard({
    task,
    model,
}: {
    task: BackupTask
    model: ConsoleModel
}) {
    const { records, scans, backups, busy, connection } = model
    const scan = scans[task.id]
    const preparing = model.preparingScanId === task.id
    const running = scan?.state === 'RUNNING'
    const scanLost =
        running &&
        model.operationConnection.id === scan.id &&
        model.operationConnection.lost
    const backupOperation = backups[task.id]
    const preparingBackup = model.preparingBackupId === task.id
    const backingUp = preparingBackup || backupOperation?.state === 'RUNNING'
    const backupLost =
        backingUp &&
        model.operationConnection.id === backupOperation?.id &&
        model.operationConnection.lost
    const hasPreview = scan || preparing || backupOperation || preparingBackup
    const latest = records.find(
        (record) => record.task_id === task.id && record.action === 'backup',
    )
    const titleId = `task-${task.id}`
    const SourceIcon = !task.selection
        ? Folder
        : task.selection.length > 1
          ? Files
          : File
    const backup = () => void model.backupTask(task.id)
    return (
        <article
            className={`task-card ${hasPreview ? 'has-preview' : ''}`}
            data-task-id={task.id}
            aria-labelledby={titleId}
        >
            <header className="task-header">
                <SourceIcon className="folder-icon" size={22} />
                <div className="task-copy">
                    <h2 id={titleId}>{taskTitle(task)}</h2>
                    <span className="path">
                        {sourceLabel(task.path, task.selection)}
                    </span>
                    <small>
                        {!task.selection
                            ? '文件夹'
                            : task.selection.length === 1
                              ? entryTypeLabels[task.selection[0].type]
                              : `${task.selection.length} 项来源`}
                    </small>
                    <FileTypeSummary
                        types={task.file_types}
                        preserveEmptyDirs={task.preserve_empty_dirs}
                    />
                    <SourceDetails
                        root={task.path}
                        selection={task.selection}
                    />
                    <small>
                        {latest
                            ? `最近备份：${stateLabels[latest.state]} · ${formatTime(latest.started_at)}`
                            : '暂无近期备份记录'}
                    </small>
                </div>
                <div className="task-actions">
                    <GlassButton
                        className="scan-button"
                        disabled={busy}
                        onClick={() => void model.scanTask(task.id)}
                    >
                        {(running || preparing) && !scanLost ? (
                            <RefreshCw className="spinning" size={16} />
                        ) : (
                            <ScanLine size={16} />
                        )}
                        {scanLost
                            ? '扫描失联'
                            : running || preparing
                              ? '扫描中'
                              : scan
                                ? '重新扫描'
                                : '扫描预览'}
                    </GlassButton>
                    <GlassButton
                        className="backup-button"
                        disabled={busy || !model.canBackup}
                        title={
                            !model.canBackup ? connection.detail : '备份此任务'
                        }
                        onClick={backup}
                    >
                        {backingUp && !backupLost ? (
                            <RefreshCw className="spinning" size={16} />
                        ) : (
                            <Upload size={16} />
                        )}
                        {backupLost
                            ? '备份失联'
                            : backingUp
                              ? '备份中'
                              : '立即备份'}
                    </GlassButton>
                    <GlassButton
                        className="icon-button task-versions"
                        title="查看此任务的备份版本"
                        aria-label="查看此任务的备份版本"
                        disabled={busy}
                        onClick={() => model.viewVersions(task)}
                    >
                        <Archive size={17} />
                    </GlassButton>
                    <GlassButton
                        className="icon-button"
                        disabled={busy}
                        title="移除任务"
                        aria-label="移除任务"
                        onClick={() => model.setRemoveCandidate(task)}
                    >
                        <Trash2 size={17} />
                    </GlassButton>
                </div>
            </header>
            {preparingBackup && (
                <div className="task-backup-status" role="status">
                    <RefreshCw className="spinning" size={16} />
                    正在准备备份
                </div>
            )}
            {!preparingBackup && backupOperation && (
                <OperationPanel
                    key={backupOperation.id}
                    model={model}
                    detail={backupOperation}
                    embedded
                />
            )}
            {preparing && (
                <div className="task-scan-status" role="status">
                    <RefreshCw className="spinning" size={16} />
                    正在准备扫描
                </div>
            )}
            {!preparing && scan && (
                <div className="task-preview">
                    {scan.result ? (
                        <>
                            <ScanPreview
                                key={scan.id}
                                operationId={scan.id}
                                result={scan.result}
                                finishedAt={scan.finished_at}
                            />
                            <div className="scan-actions">
                                <GlassButton
                                    className="backup-button scan-backup"
                                    disabled={
                                        busy ||
                                        !model.canBackup ||
                                        !scan.result.complete
                                    }
                                    onClick={backup}
                                >
                                    <Upload size={16} />
                                    备份此任务
                                </GlassButton>
                            </div>
                        </>
                    ) : (
                        <section
                            className="task-scan-progress"
                            aria-live="polite"
                        >
                            <div className="section-heading">
                                <h3>
                                    扫描 ·{' '}
                                    {scanLost
                                        ? '状态失联'
                                        : stateLabels[scan.state]}
                                </h3>
                                <span className="muted">
                                    {scan.files} 个文件 ·{' '}
                                    {formatBytes(scan.bytes)}
                                </span>
                            </div>
                            {running && !scanLost && (
                                <progress aria-label="扫描进行中" />
                            )}
                            <OperationConnection id={scan.id} model={model} />
                            {scan.path && (
                                <p className="path muted">{scan.path}</p>
                            )}
                            {scan.error && (
                                <p className="error-text">{scan.error}</p>
                            )}
                        </section>
                    )}
                </div>
            )}
        </article>
    )
}
