import {
    Archive,
    ArrowDownToLine,
    FolderOpen,
    RefreshCw,
    Trash2,
    X,
} from 'lucide-react'
import { useEffect, useRef, type ReactNode } from 'react'
import type { Operation } from './types'
import type { ConsoleModel } from './use-backup-console'
import { WarningDetails } from './warning-details'
import { RestoreDetails } from './restore-details'
import { OperationConnection } from './operation-connection'
import {
    actionLabels,
    formatBytes,
    formatTime,
    stageLabels,
    stateLabels,
} from './ui'

function Dialog({
    title,
    onClose,
    children,
}: {
    title: string
    onClose: () => void
    children: ReactNode
}) {
    const ref = useRef<HTMLDialogElement>(null)
    useEffect(() => {
        const dialog = ref.current!
        dialog.showModal()
        return () => dialog.close()
    }, [])
    return (
        <dialog
            ref={ref}
            className="workflow-dialog"
            aria-labelledby="dialog-title"
            onCancel={onClose}
        >
            <div className="section-heading">
                <h2 id="dialog-title">{title}</h2>
                <button
                    className="icon-button"
                    aria-label="关闭对话框"
                    title="关闭"
                    onClick={onClose}
                >
                    <X size={18} />
                </button>
            </div>
            {children}
        </dialog>
    )
}

export function WorkflowDialogs({ model }: { model: ConsoleModel }) {
    const { removeCandidate, restoreCandidate, restoreDestination, busy } =
        model
    return (
        <>
            {removeCandidate && (
                <Dialog
                    title="移除备份任务"
                    onClose={() => model.setRemoveCandidate(null)}
                >
                    <p className="path">{removeCandidate.path}</p>
                    <p>只移除任务配置，源文件和已完成的备份版本都会保留。</p>
                    <div className="dialog-actions">
                        <button
                            autoFocus
                            onClick={() => model.setRemoveCandidate(null)}
                        >
                            取消
                        </button>
                        <button
                            className="danger-button"
                            disabled={busy}
                            onClick={() => void model.removeTask()}
                        >
                            <Trash2 size={16} />
                            移除任务
                        </button>
                    </div>
                </Dialog>
            )}
            {restoreCandidate && (
                <Dialog
                    title="还原备份版本"
                    onClose={() => model.setRestoreCandidate(null)}
                >
                    <dl className="restore-summary">
                        <dt>备份时间</dt>
                        <dd>{formatTime(restoreCandidate.completed_at)}</dd>
                        <dt>源目录</dt>
                        <dd className="path">{restoreCandidate.source}</dd>
                        <dt>备份内容</dt>
                        <dd>
                            {restoreCandidate.files} 个文件 ·{' '}
                            {formatBytes(restoreCandidate.bytes)}
                        </dd>
                    </dl>
                    <label className="restore-location">
                        还原到
                        <div className="directory-input">
                            <input
                                readOnly
                                value={restoreDestination}
                                placeholder="选择新建或空目录"
                                aria-label="还原目录"
                            />
                            <button
                                title="选择还原目录"
                                aria-label="选择还原目录"
                                disabled={busy}
                                onClick={() =>
                                    void model.chooseRestoreDirectory()
                                }
                            >
                                <FolderOpen size={18} />
                            </button>
                        </div>
                    </label>
                    <p className="muted">
                        目标必须为空，不能与源目录或备份仓库重叠。
                    </p>
                    {model.message && (
                        <p className="error-text" role="alert">
                            {model.message}
                        </p>
                    )}
                    <div className="dialog-actions">
                        <button
                            autoFocus
                            onClick={() => model.setRestoreCandidate(null)}
                        >
                            取消
                        </button>
                        <button
                            className="backup-button confirm-restore"
                            disabled={busy || !restoreDestination}
                            onClick={() => void model.restore()}
                        >
                            <ArrowDownToLine size={16} />
                            开始还原
                        </button>
                    </div>
                </Dialog>
            )}
        </>
    )
}

export function OperationPanel({
    model,
    detail,
    embedded = false,
}: {
    model: ConsoleModel
    detail: Operation
    embedded?: boolean
}) {
    const { busy } = model
    const running = detail.state === 'RUNNING'
    const lost =
        running &&
        model.operationConnection.id === detail.id &&
        model.operationConnection.lost
    const succeeded = detail.state.startsWith('SUCCEEDED')
    const Heading = embedded ? 'h3' : 'h2'
    return (
        <section
            className={`operation-panel state-${detail.state.toLowerCase()}`}
            aria-live="polite"
        >
            <div className="section-heading">
                <Heading>
                    {actionLabels[detail.action]} ·{' '}
                    {lost ? '状态失联' : stateLabels[detail.state]}
                </Heading>
                <div className="inline-actions">
                    <span>
                        {running && `${stageLabels[detail.stage]} · `}
                        {detail.files} 个文件 · {formatBytes(detail.bytes)}
                    </span>
                    {(!running || model.tab === 'records') && (
                        <button
                            className="icon-button"
                            aria-label="收起执行结果"
                            title="收起执行结果"
                            onClick={() => model.closeDetail(detail)}
                        >
                            <X size={16} />
                        </button>
                    )}
                </div>
            </div>
            {running && !lost && <progress aria-label="操作进行中" />}
            <OperationConnection id={detail.id} model={model} />
            <p className="path">{detail.destination || detail.source}</p>
            {running && detail.path && (
                <small className="path">{detail.path}</small>
            )}
            {detail.error && <p className="error-text">{detail.error}</p>}
            <div className="result-actions">
                {succeeded && detail.action === 'backup' && (
                    <button
                        className="view-versions"
                        onClick={() =>
                            model.viewVersions(
                                model.config.tasks.find(
                                    (task) => task.id === detail.task_id,
                                ),
                            )
                        }
                    >
                        <Archive size={16} />
                        查看备份版本
                    </button>
                )}
                {detail.state === 'WAITING' && (
                    <button
                        disabled={busy}
                        onClick={() => void model.confirmOperation(detail)}
                    >
                        <RefreshCw size={16} />
                        确认提交结果
                    </button>
                )}
                <details className="operation-details">
                    <summary>执行详情</summary>
                    <small>执行编号：{detail.id}</small>
                    <small>开始时间：{formatTime(detail.started_at)}</small>
                    {detail.finished_at && (
                        <small>
                            结束时间：{formatTime(detail.finished_at)}
                        </small>
                    )}
                </details>
            </div>
            {!!detail.warning_count && (
                <WarningDetails
                    key={detail.id}
                    operationId={detail.id}
                    count={detail.warning_count}
                    label={detail.error_count ? '项问题' : '项警告'}
                />
            )}
            {detail.restore_journal && !running && (
                <RestoreDetails key={detail.id} operationId={detail.id} />
            )}
        </section>
    )
}
