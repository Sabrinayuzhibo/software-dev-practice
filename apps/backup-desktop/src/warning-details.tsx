import { GlassButton } from './glass-controls'
import { usePagedDetails } from './use-paged-details'
import { RefreshCw } from 'lucide-react'
import type { Warning, WarningPage } from './types'

// 调用处按操作 ID 设置 key，新的扫描从第一页开始。
export function WarningDetails({
    operationId,
    loadPage,
    count,
    className = '',
    label = '项警告',
}: {
    operationId?: string
    loadPage?: (offset: number) => Promise<WarningPage>
    count: number
    className?: string
    label?: string
}) {
    const {
        items: warnings,
        nextOffset,
        loading,
        error,
        load,
    } = usePagedDetails<Warning, WarningPage>(
        (offset) =>
            loadPage
                ? loadPage(offset)
                : window.backup.getOperationWarnings(operationId!, offset),
        (page) => page.warnings,
    )

    return (
        <details
            className={`warning-details ${className}`}
            onToggle={(event) => {
                if (event.currentTarget.open && nextOffset === 0) {
                    void load()
                }
            }}
        >
            <summary>
                {count} {label}
            </summary>
            <div className="warning-list">
                {warnings.map((warning, index) => (
                    <p
                        className={
                            warning.severity === 'error'
                                ? 'path error-text'
                                : 'path'
                        }
                        key={index}
                    >
                        {warning.path}：{warning.reason}
                    </p>
                ))}
            </div>
            {error && (
                <p className="error-text" role="alert">
                    读取警告失败：{error}
                </p>
            )}
            <div className="inline-actions warning-pagination">
                <small className="muted">
                    已显示 {warnings.length} / {count}
                </small>
                {nextOffset !== null && (
                    <GlassButton disabled={loading} onClick={() => void load()}>
                        <RefreshCw
                            size={14}
                            className={loading ? 'spinning' : ''}
                        />
                        {loading ? '加载中' : error ? '重试' : '加载更多'}
                    </GlassButton>
                )}
            </div>
        </details>
    )
}
