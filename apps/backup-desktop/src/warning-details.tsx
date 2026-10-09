import { GlassButton } from './glass-controls'
import { useEffect, useRef, useState } from 'react'
import { RefreshCw } from 'lucide-react'
import type { Warning, WarningPage } from './types'

// Key by operation ID at the call site so a new scan starts with a fresh page.
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
    const [warnings, setWarnings] = useState<Warning[]>([])
    const [nextOffset, setNextOffset] = useState<number | null>(0)
    const [loading, setLoading] = useState(false)
    const [error, setError] = useState('')
    const pending = useRef(false)
    const mounted = useRef(true)
    useEffect(() => {
        mounted.current = true
        return () => {
            mounted.current = false
        }
    }, [])

    async function load() {
        if (pending.current || nextOffset === null) {
            return
        }
        pending.current = true
        setLoading(true)
        setError('')
        try {
            const page = loadPage
                ? await loadPage(nextOffset)
                : await window.backup.getOperationWarnings(
                      operationId!,
                      nextOffset,
                  )
            if (mounted.current) {
                setWarnings((old) => [...old, ...page.warnings])
                setNextOffset(page.next_offset)
            }
        } catch (failure) {
            if (mounted.current) {
                setError(String(failure))
            }
        } finally {
            pending.current = false
            if (mounted.current) {
                setLoading(false)
            }
        }
    }

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
