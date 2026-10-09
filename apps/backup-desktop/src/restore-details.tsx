import { GlassButton } from './glass-controls'
import { useEffect, useRef, useState } from 'react'
import { RefreshCw } from 'lucide-react'
import type { RestorePage } from './types'
import { entryTypeLabels } from './ui'

export function RestoreDetails({ operationId }: { operationId: string }) {
    const [entries, setEntries] = useState<RestorePage['entries']>([])
    const [page, setPage] = useState<RestorePage | null>(null)
    const [error, setError] = useState('')
    const [loading, setLoading] = useState(false)
    const pending = useRef(false)
    const mounted = useRef(true)
    useEffect(() => {
        mounted.current = true
        return () => {
            mounted.current = false
        }
    }, [])
    async function load(offset: number) {
        if (pending.current) {
            return
        }
        pending.current = true
        setLoading(true)
        setError('')
        try {
            const result = await window.backup.getRestoreEntries(
                operationId,
                offset,
            )
            if (mounted.current) {
                setEntries((old) =>
                    offset ? [...old, ...result.entries] : result.entries,
                )
                setPage(result)
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
            className="restore-details"
            onToggle={(event) => {
                if (event.currentTarget.open && !page) {
                    void load(0)
                }
            }}
        >
            <summary>还原写入明细</summary>
            {page && (
                <p>
                    已写入 {page.written} 项 · 待核对 {page.uncertain} 项
                </p>
            )}
            {page?.incomplete && (
                <p className="error-text">
                    中断时明细未完整落盘，最后一项结果待核对。
                </p>
            )}
            <div className="warning-list">
                {entries.map((entry) => (
                    <p className="path" key={entry.path}>
                        {entry.path} ·{' '}
                        {entryTypeLabels[entry.type] || '未知类型'} ·{' '}
                        {entry.state === 'written' ? '已写入' : '结果待核对'}
                        {entry.temporary_path && (
                            <small>
                                可能残留的临时文件：{entry.temporary_path}
                            </small>
                        )}
                    </p>
                ))}
            </div>
            {error && (
                <p role="alert" className="error-text">
                    {error}
                </p>
            )}
            {(!page || page.next_offset !== null) && (
                <GlassButton
                    disabled={loading}
                    onClick={() => void load(page?.next_offset || 0)}
                >
                    <RefreshCw
                        size={14}
                        className={loading ? 'spinning' : ''}
                    />
                    {loading ? '加载中' : error ? '重试' : '加载更多'}
                </GlassButton>
            )}
        </details>
    )
}
