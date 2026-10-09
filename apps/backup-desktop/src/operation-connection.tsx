import { GlassButton } from './glass-controls'
import { RefreshCw } from 'lucide-react'
import type { ConsoleModel } from './use-backup-console'

export function OperationConnection({
    id,
    model,
}: {
    id: string
    model: ConsoleModel
}) {
    const status = model.operationConnection
    if (status.id !== id || !status.error) {
        return null
    }
    return (
        <div className="operation-connection" role="alert">
            <p className="error-text">
                {status.lost
                    ? 'Agent 状态失联，执行结果未知。恢复连接前暂停新操作。'
                    : '暂时无法读取执行状态，正在重试。'}
            </p>
            {status.lost && (
                <GlassButton onClick={model.retryOperation}>
                    <RefreshCw size={14} />
                    重新查询状态
                </GlassButton>
            )}
            <details>
                <summary>连接详情</summary>
                <p className="path">{status.error}</p>
            </details>
        </div>
    )
}
