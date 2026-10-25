import { Trash2 } from 'lucide-react'
import { GlassButton } from './glass-controls'
import { Dialog } from './workflow'
import { SourceDetails, sourceLabel } from './source-scope'
import { formatBytes, formatTime } from './ui'
import type { ConsoleModel } from './use-backup-console'

export function DeleteVersionDialog({ model }: { model: ConsoleModel }) {
    const candidate = model.deleteCandidate
    if (!candidate) {
        return null
    }
    const { version, target, repositoryPath } = candidate
    const close = () => {
        if (!model.busy) {
            model.setDeleteCandidate(null)
        }
    }
    return (
        <Dialog
            title="删除备份版本"
            onClose={close}
            initialFocus=".cancel-delete-version"
        >
            <dl className="restore-summary delete-version-summary">
                <dt>备份目标</dt>
                <dd>{target.name}</dd>
                {repositoryPath && (
                    <>
                        <dt>仓库位置</dt>
                        <dd className="path">{repositoryPath}</dd>
                    </>
                )}
                {version.completed_at && (
                    <>
                        <dt>备份时间</dt>
                        <dd>{formatTime(version.completed_at)}</dd>
                    </>
                )}
                <dt>版本编号</dt>
                <dd className="path">{version.id}</dd>
                {version.source && (
                    <>
                        <dt>备份来源</dt>
                        <dd className="path">
                            {version.source_name && (
                                <strong>{version.source_name}</strong>
                            )}
                            <div>
                                {sourceLabel(version.source, version.selection)}
                            </div>
                            <SourceDetails
                                root={version.source}
                                selection={version.selection}
                            />
                        </dd>
                    </>
                )}
                {version.files !== undefined && version.bytes !== undefined && (
                    <>
                        <dt>备份内容</dt>
                        <dd>
                            {version.files} 个文件 ·{' '}
                            {formatBytes(version.bytes)}
                        </dd>
                    </>
                )}
            </dl>
            <p className="error-text">
                删除后无法再从此版本还原，且无法撤销。源文件、任务配置和其他备份版本会保留。
            </p>
            {model.message && (
                <p className="error-text" role="alert">
                    {model.message}
                </p>
            )}
            <div className="dialog-actions">
                <GlassButton
                    className="cancel-delete-version"
                    disabled={model.busy}
                    onClick={close}
                >
                    取消
                </GlassButton>
                <GlassButton
                    className="danger-button confirm-delete-version"
                    disabled={model.busy}
                    onClick={() => void model.deleteVersion()}
                >
                    <Trash2 size={16} />
                    {model.busy
                        ? '正在提交'
                        : candidate.retryOperationId
                          ? '重试删除'
                          : '删除此版本'}
                </GlassButton>
            </div>
        </Dialog>
    )
}
