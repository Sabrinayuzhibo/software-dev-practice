import {
    Archive,
    ArrowDownToLine,
    Database,
    Folder,
    Plus,
    RefreshCw,
    Settings,
} from 'lucide-react'
import { useState } from 'react'

import { formatBytes, formatTime, specialEntryCounts } from './ui'
import type { ConsoleModel } from './use-backup-console'
import { TaskCard } from './task-card'
import { WarningDetails } from './warning-details'
import { CreateTaskDialog } from './create-task-dialog'
import { SourceDetails, sourceLabel, taskTitle } from './source-scope'
import { FileTypeSummary } from './file-type-filter'

export function TargetsView({ model }: { model: ConsoleModel }) {
    const { config, targetDraft, setTargetDraft, mutating, saveTarget } = model
    return (
        <section>
            <p className="muted">
                仓库与源文件位于同一磁盘时，备份无法抵御该磁盘损坏。
            </p>
            <div className="table-scroll">
                <table>
                    <thead>
                        <tr>
                            <th>名称</th>
                            <th>服务地址</th>
                            <th>仓库</th>
                            <th>操作</th>
                        </tr>
                    </thead>
                    <tbody>
                        {config.targets.map((item) => (
                            <tr key={item.id}>
                                <td>
                                    <Database size={16} /> {item.name}
                                </td>
                                <td>
                                    {item.host}:{item.port}
                                </td>
                                <td className="path">
                                    {item.repository_path ||
                                        item.data_root ||
                                        '由本机服务管理'}
                                </td>
                                <td>
                                    <button
                                        disabled={mutating}
                                        onClick={() => setTargetDraft(item)}
                                    >
                                        <Settings size={16} />
                                        编辑
                                    </button>
                                </td>
                            </tr>
                        ))}
                    </tbody>
                </table>
            </div>
            <form
                className="target-form"
                onSubmit={(event) => void saveTarget(event)}
            >
                <h2>{targetDraft.id ? '编辑目标' : '新建目标'}</h2>
                <label>
                    名称
                    <input
                        required
                        maxLength={100}
                        value={targetDraft.name}
                        onChange={(event) =>
                            setTargetDraft({
                                ...targetDraft,
                                name: event.target.value,
                            })
                        }
                    />
                </label>
                <label>
                    主机
                    <input value="127.0.0.1" readOnly />
                </label>
                <label>
                    模式
                    <select value="local" disabled>
                        <option value="local">本地仓库</option>
                    </select>
                </label>
                <label className="repository-path">
                    仓库路径（可选）
                    <input
                        value={targetDraft.repository_path || ''}
                        placeholder="留空使用当前服务的仓库"
                        onChange={(event) =>
                            setTargetDraft({
                                ...targetDraft,
                                repository_path: event.target.value,
                            })
                        }
                    />
                </label>
                <label>
                    端口
                    <input
                        type="number"
                        min={1}
                        max={65535}
                        required
                        value={targetDraft.port}
                        onChange={(event) =>
                            setTargetDraft({
                                ...targetDraft,
                                port: Number(event.target.value),
                            })
                        }
                    />
                </label>
                <div className="form-actions">
                    <button className="backup-button" disabled={mutating}>
                        保存目标
                    </button>
                    {targetDraft.id && (
                        <button
                            type="button"
                            onClick={() =>
                                setTargetDraft({
                                    name: '',
                                    host: '127.0.0.1',
                                    port: 9000,
                                })
                            }
                        >
                            取消编辑
                        </button>
                    )}
                </div>
            </form>
        </section>
    )
}

export function VersionsView({ model }: { model: ConsoleModel }) {
    const {
        versions,
        versionTotal,
        versionNextOffset,
        versionError,
        versionsLoading,
        busy,
        loadVersions,
        requestRestore,
        versionTaskId,
        setVersionTaskId,
        tasks,
        connection,
    } = model
    return (
        <section>
            <div className="section-heading">
                <label className="version-filter">
                    任务
                    <select
                        aria-label="筛选备份任务"
                        value={versionTaskId}
                        disabled={versionsLoading}
                        onChange={(event) =>
                            setVersionTaskId(event.target.value)
                        }
                    >
                        <option value="">全部任务（含已移除任务）</option>
                        {tasks.map((task) => (
                            <option key={task.id} value={task.id}>
                                {taskTitle(task)}
                            </option>
                        ))}
                    </select>
                </label>
                <button
                    disabled={
                        busy ||
                        versionsLoading ||
                        connection.state === 'checking'
                    }
                    onClick={() => void loadVersions()}
                >
                    <RefreshCw size={16} />
                    刷新
                </button>
            </div>
            {!versionError &&
                !versionsLoading &&
                ['online', 'storage-unavailable'].includes(
                    connection.state,
                ) && <p className="muted">{versionTotal} 个已完成版本</p>}
            {versionError && (
                <p role="alert" className="error-text">
                    {versionError}
                </p>
            )}
            {versionsLoading && <p className="muted">正在查询版本</p>}
            {connection.state === 'offline' && !versionError && (
                <p className="error-text">服务未连接，暂时无法查询备份版本。</p>
            )}
            {!versionError &&
                !versionsLoading &&
                ['online', 'storage-unavailable'].includes(connection.state) &&
                versions.length === 0 && (
                    <div className="empty-state">
                        <Archive size={32} />
                        <p>暂无备份版本</p>
                        <button onClick={() => model.navigate('tasks')}>
                            <Folder size={16} />
                            前往备份任务
                        </button>
                    </div>
                )}
            <div className="table-scroll">
                <table className="versions-table">
                    <thead>
                        <tr>
                            <th>完成时间 / 版本</th>
                            <th>备份来源</th>
                            <th>内容</th>
                            <th>操作</th>
                        </tr>
                    </thead>
                    <tbody>
                        {versions.map((version) => (
                            <tr key={version.id}>
                                <td>
                                    {formatTime(version.completed_at)}
                                    <small title={version.id}>
                                        版本 {version.id.slice(0, 8)}
                                    </small>
                                </td>
                                <td className="path">
                                    {version.source_name && (
                                        <strong>{version.source_name}</strong>
                                    )}
                                    <div>
                                        {sourceLabel(
                                            version.source,
                                            version.selection,
                                        )}
                                    </div>
                                    <SourceDetails
                                        root={version.source}
                                        selection={version.selection}
                                    />
                                    {!!version.warning_count && (
                                        <WarningDetails
                                            key={`${model.targetId}:${version.id}`}
                                            count={version.warning_count}
                                            loadPage={(offset) =>
                                                window.backup.getVersionWarnings(
                                                    model.targetId,
                                                    version.id,
                                                    offset,
                                                )
                                            }
                                        />
                                    )}
                                </td>
                                <td>
                                    {version.files} 个文件
                                    <small>
                                        {version.directories} 个目录 ·{' '}
                                        {formatBytes(version.bytes)}
                                    </small>
                                    <small>
                                        {version.selection
                                            ? `范围：选定 ${version.selection.length} 项`
                                            : '范围：完整目录'}
                                    </small>
                                    <FileTypeSummary
                                        types={version.file_types}
                                        preserveEmptyDirs={
                                            version.preserve_empty_dirs
                                        }
                                    />
                                    {specialEntryCounts(version) && (
                                        <small>
                                            {specialEntryCounts(version)}
                                        </small>
                                    )}
                                    {Number(version.hardlinks) > 0 &&
                                        version.stored_bytes !== undefined && (
                                            <small>
                                                内容存储：
                                                {formatBytes(
                                                    version.stored_bytes,
                                                )}
                                            </small>
                                        )}
                                </td>
                                <td>
                                    <button
                                        disabled={busy || versionsLoading}
                                        onClick={() => requestRestore(version)}
                                    >
                                        <ArrowDownToLine size={16} />
                                        还原到…
                                    </button>
                                </td>
                            </tr>
                        ))}
                    </tbody>
                </table>
            </div>
            {versionNextOffset !== null && (
                <button
                    disabled={busy || versionsLoading}
                    onClick={() => void loadVersions(versionNextOffset)}
                >
                    加载更多
                </button>
            )}
        </section>
    )
}

export function TasksView({ model }: { model: ConsoleModel }) {
    const { tasks, busy } = model
    const [creating, setCreating] = useState(false)
    return (
        <section>
            <div className="section-heading">
                <span className="muted">{tasks.length} 个任务</span>
                <button
                    className="primary-button"
                    disabled={busy}
                    onClick={() => setCreating(true)}
                >
                    <Plus size={17} />
                    新建任务
                </button>
            </div>
            {tasks.length === 0 && (
                <div className="empty-state">
                    <Folder size={32} />
                    <p>当前目标下暂无备份任务</p>
                </div>
            )}
            <div className="task-list">
                {tasks.map((task) => (
                    <TaskCard key={task.id} task={task} model={model} />
                ))}
            </div>
            {creating && (
                <CreateTaskDialog
                    model={model}
                    onClose={() => setCreating(false)}
                />
            )}
        </section>
    )
}
