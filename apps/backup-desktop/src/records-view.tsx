import { ChevronDown, ChevronRight, Clock, RefreshCw } from 'lucide-react'
import { Fragment, useEffect, useRef, useState } from 'react'
import type { Operation } from './types'
import type { ConsoleModel } from './use-backup-console'
import { actionLabels, formatBytes, formatTime, stateLabels } from './ui'
import { sourceLabel } from './source-scope'
import { FileTypeSummary } from './file-type-filter'

function RecordCells({
    record,
    showRecord,
    lost,
}: {
    record: Operation
    showRecord: (id: string) => Promise<void>
    lost: boolean
}) {
    return (
        <>
            <td>{formatTime(record.started_at)}</td>
            <td>
                {actionLabels[record.action]}
                {record.source_name && <small>{record.source_name}</small>}
                <small className="path">
                    {record.destination ||
                        (record.source &&
                            sourceLabel(record.source, record.selection))}
                </small>
                {record.selection && (
                    <small>选定 {record.selection.length} 项</small>
                )}
                <FileTypeSummary
                    types={record.file_types}
                    preserveEmptyDirs={record.preserve_empty_dirs}
                />
                {!!record.scan_count && (
                    <small className="scan-history-count">
                        <span>共 {record.scan_count} 次</span>
                        <span title="失败或中断的扫描次数">
                            {record.failure_count} 次异常
                        </span>
                        <span>{record.warning_runs} 次有警告</span>
                    </small>
                )}
            </td>
            <td>
                <span
                    className={`state-label state-label-${record.state.toLowerCase()}`}
                >
                    {lost ? '状态失联' : stateLabels[record.state]}
                </span>
                {!!record.warning_count && (
                    <small>{record.warning_count} 项警告</small>
                )}
            </td>
            <td>
                {record.files} 个文件
                <small>{formatBytes(record.bytes)}</small>
            </td>
            <td>
                <button
                    className="record-view"
                    onClick={() => void showRecord(record.id)}
                >
                    查看
                </button>
            </td>
        </>
    )
}

function RecordRow({
    record,
    model,
}: {
    record: Operation
    model: ConsoleModel
}) {
    const [expanded, setExpanded] = useState(false)
    const [history, setHistory] = useState<Operation[]>([])
    const [nextOffset, setNextOffset] = useState<number | null>(0)
    const [loading, setLoading] = useState(false)
    const [error, setError] = useState('')
    const request = useRef(0)
    const pending = useRef(false)
    const group = record.scan_group

    async function load(offset: number) {
        if (!group || pending.current) {
            return
        }
        const current = ++request.current
        pending.current = true
        setLoading(true)
        setError('')
        try {
            const result = await window.backup.listOperations(
                offset,
                false,
                group,
            )
            if (request.current === current) {
                setHistory((old) =>
                    offset ? [...old, ...result.operations] : result.operations,
                )
                setNextOffset(result.next_offset)
            }
        } catch (failure) {
            if (request.current === current) {
                setError(String(failure))
            }
        } finally {
            if (request.current === current) {
                pending.current = false
                setLoading(false)
            }
        }
    }

    useEffect(() => {
        if (expanded) {
            setHistory([])
            setNextOffset(0)
            void load(0)
        }
        return () => {
            ++request.current
            pending.current = false
        }
    }, [expanded, record.id, record.state, record.scan_count])

    // Keep an expanded latest row current without refetching its whole history.
    const currentHistory = history.map((item) =>
        item.id === record.id
            ? {
                  ...item,
                  state: record.state,
                  files: record.files,
                  bytes: record.bytes,
                  warning_count: record.warning_count,
              }
            : item,
    )
    return (
        <Fragment>
            <tr
                data-operation-id={record.id}
                className={group ? 'scan-group' : undefined}
            >
                <td className="record-toggle-cell">
                    {group && (
                        <button
                            className="icon-button scan-group-toggle"
                            aria-label={
                                expanded ? '收起扫描历史' : '展开扫描历史'
                            }
                            title={expanded ? '收起扫描历史' : '展开扫描历史'}
                            aria-expanded={expanded}
                            onClick={() => setExpanded(!expanded)}
                        >
                            {expanded ? (
                                <ChevronDown size={16} />
                            ) : (
                                <ChevronRight size={16} />
                            )}
                        </button>
                    )}
                </td>
                <RecordCells
                    record={record}
                    showRecord={model.showRecord}
                    lost={
                        record.state === 'RUNNING' &&
                        model.operationConnection.id === record.id &&
                        model.operationConnection.lost
                    }
                />
            </tr>
            {expanded &&
                currentHistory.map((item) => (
                    <tr
                        key={item.id}
                        className="scan-history-row"
                        data-operation-id={item.id}
                    >
                        <td />
                        <RecordCells
                            record={item}
                            showRecord={model.showRecord}
                            lost={
                                item.state === 'RUNNING' &&
                                model.operationConnection.id === item.id &&
                                model.operationConnection.lost
                            }
                        />
                    </tr>
                ))}
            {expanded && (loading || error || nextOffset !== null) && (
                <tr className="scan-history-pagination">
                    <td />
                    <td colSpan={5}>
                        {error && (
                            <p role="alert" className="error-text">
                                读取扫描历史失败：{error}
                            </p>
                        )}
                        <button
                            disabled={loading}
                            onClick={() => void load(nextOffset ?? 0)}
                        >
                            <RefreshCw
                                size={14}
                                className={loading ? 'spinning' : ''}
                            />
                            {loading
                                ? '加载中'
                                : error
                                  ? '重试'
                                  : '加载更多扫描记录'}
                        </button>
                    </td>
                </tr>
            )}
        </Fragment>
    )
}

export function RecordsView({ model }: { model: ConsoleModel }) {
    const {
        records,
        recordTotal,
        recordNextOffset,
        recordsLoading,
        loadRecords,
    } = model
    return (
        <section>
            <div className="section-heading">
                <span className="muted">{recordTotal} 条执行记录</span>
                <button
                    className="records-refresh"
                    disabled={recordsLoading}
                    onClick={() => void loadRecords()}
                >
                    <RefreshCw size={16} />
                    {recordsLoading ? '加载中' : '刷新'}
                </button>
            </div>
            {records.length === 0 && (
                <div className="empty-state">
                    <Clock size={32} />
                    <p>暂无执行记录</p>
                </div>
            )}
            <div className="table-scroll">
                <table className="records-table">
                    <thead>
                        <tr>
                            <th
                                className="record-toggle-cell"
                                aria-label="展开历史"
                            />
                            <th>执行时间</th>
                            <th>操作</th>
                            <th>状态</th>
                            <th>处理量</th>
                            <th>详情</th>
                        </tr>
                    </thead>
                    <tbody>
                        {records.map((record) => (
                            <RecordRow
                                key={record.scan_group || record.id}
                                record={record}
                                model={model}
                            />
                        ))}
                    </tbody>
                </table>
            </div>
            {recordNextOffset !== null && (
                <button
                    disabled={recordsLoading}
                    onClick={() => void loadRecords(recordNextOffset)}
                >
                    加载更多
                </button>
            )}
        </section>
    )
}
