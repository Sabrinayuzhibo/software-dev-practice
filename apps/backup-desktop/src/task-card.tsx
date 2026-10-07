import {
    Archive,
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
    const backupOperation = backups[task.id]
    const preparingBackup = model.preparingBackupId === task.id
    const backingUp = preparingBackup || backupOperation?.state === 'RUNNING'
    const hasPreview = scan || preparing || backupOperation || preparingBackup
    const latest = records.find(
        (record) => record.task_id === task.id && record.action === 'backup',
    )
    const titleId = `task-${task.id}`
    const backup = () => void model.backupTask(task.id)
    return (
        <article
            className={`task-card ${hasPreview ? 'has-preview' : ''}`}
            data-task-id={task.id}
            aria-labelledby={titleId}
        >
            <header className="task-header">
                <Folder className="folder-icon" size={22} />
                <div className="task-copy">
                    <h2 id={titleId}>
                        {task.path.split('/').pop() || task.path}
                    </h2>
                    <span className="path">{task.path}</span>
                    <small>
                        {latest
                            ? `最近备份：${stateLabels[latest.state]} · ${formatTime(latest.started_at)}`
                            : '暂无近期备份记录'}
                    </small>
                </div>
                <div className="task-actions">
                    <button
                        className="scan-button"
                        disabled={busy}
                        onClick={() => void model.scanTask(task.id)}
                    >
                        {running || preparing ? (
                            <RefreshCw className="spinning" size={16} />
                        ) : (
                            <ScanLine size={16} />
                        )}
                        {running || preparing
                            ? '扫描中'
                            : scan
                              ? '重新扫描'
                              : '扫描预览'}
                    </button>
                    <button
                        className="backup-button"
                        disabled={busy || connection.state === 'offline'}
                        title={
                            connection.state === 'offline'
                                ? '备份服务未连接'
                                : '备份此目录'
                        }
                        onClick={backup}
                    >
                        {backingUp ? (
                            <RefreshCw className="spinning" size={16} />
                        ) : (
                            <Upload size={16} />
                        )}
                        {backingUp ? '备份中' : '立即备份'}
                    </button>
                    <button
                        className="icon-button task-versions"
                        title="查看此目录的备份版本"
                        aria-label="查看此目录的备份版本"
                        disabled={busy}
                        onClick={() => model.viewVersions(task)}
                    >
                        <Archive size={17} />
                    </button>
                    <button
                        className="icon-button"
                        disabled={busy}
                        title="移除任务"
                        aria-label="移除任务"
                        onClick={() => model.setRemoveCandidate(task)}
                    >
                        <Trash2 size={17} />
                    </button>
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
                    {scan.result && scan.state.startsWith('SUCCEEDED') ? (
                        <>
                            <ScanPreview
                                key={scan.id}
                                operationId={scan.id}
                                result={scan.result}
                                finishedAt={scan.finished_at}
                            />
                            <div className="scan-actions">
                                <button
                                    className="backup-button scan-backup"
                                    disabled={
                                        busy || connection.state === 'offline'
                                    }
                                    onClick={backup}
                                >
                                    <Upload size={16} />
                                    备份此目录
                                </button>
                            </div>
                        </>
                    ) : (
                        <section
                            className="task-scan-progress"
                            aria-live="polite"
                        >
                            <div className="section-heading">
                                <h3>扫描 · {stateLabels[scan.state]}</h3>
                                <span className="muted">
                                    {scan.files} 个文件 ·{' '}
                                    {formatBytes(scan.bytes)}
                                </span>
                            </div>
                            {running && <progress aria-label="扫描进行中" />}
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
