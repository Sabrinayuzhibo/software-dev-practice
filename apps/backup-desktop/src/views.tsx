import {
    Archive,
    ArrowDownToLine,
    Database,
    Folder,
    FolderPlus,
    RefreshCw,
    Settings,
} from 'lucide-react'

import { formatBytes, formatTime } from './ui'
import type { ConsoleModel } from './use-backup-console'
import { TaskCard } from './task-card'

export function TargetsView({ model }: { model: ConsoleModel }) {
    const { config, targetDraft, setTargetDraft, busy, saveTarget } = model
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
                                <td>由本机服务管理</td>
                                <td>
                                    <button
                                        disabled={busy}
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
                    <button className="backup-button" disabled={busy}>
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
                    目录
                    <select
                        aria-label="筛选备份目录"
                        value={versionTaskId}
                        disabled={versionsLoading}
                        onChange={(event) =>
                            setVersionTaskId(event.target.value)
                        }
                    >
                        <option value="">全部目录（含已移除任务）</option>
                        {tasks.map((task) => (
                            <option key={task.id} value={task.id}>
                                {task.path}
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
                connection.state === 'online' && (
                    <p className="muted">{versionTotal} 个已完成版本</p>
                )}
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
                connection.state === 'online' &&
                versions.length === 0 && (
                    <div className="empty-state">
                        <Archive size={32} />
                        <p>暂无备份版本</p>
                        <button onClick={() => model.navigate('tasks')}>
                            <Folder size={16} />
                            前往目录任务
                        </button>
                    </div>
                )}
            <div className="table-scroll">
                <table className="versions-table">
                    <thead>
                        <tr>
                            <th>完成时间 / 版本</th>
                            <th>源目录</th>
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
                                    {version.source}
                                    {!!version.warnings.length && (
                                        <details>
                                            <summary>
                                                {version.warnings.length} 项警告
                                            </summary>
                                            {version.warnings.map(
                                                (item, index) => (
                                                    <p key={index}>
                                                        {item.path}：
                                                        {item.reason}
                                                    </p>
                                                ),
                                            )}
                                        </details>
                                    )}
                                </td>
                                <td>
                                    {version.files} 个文件
                                    <small>
                                        {version.directories} 个目录 ·{' '}
                                        {formatBytes(version.bytes)}
                                    </small>
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
            {versions.length < versionTotal && (
                <button
                    disabled={busy || versionsLoading}
                    onClick={() => void loadVersions(versions.length)}
                >
                    加载更多
                </button>
            )}
        </section>
    )
}

export function TasksView({ model }: { model: ConsoleModel }) {
    const { tasks, busy, addFolder } = model
    return (
        <section>
            <div className="section-heading">
                <span className="muted">{tasks.length} 个目录</span>
                <button
                    className="primary-button"
                    disabled={busy}
                    onClick={() => void addFolder()}
                >
                    <FolderPlus size={17} />
                    添加目录
                </button>
            </div>
            {tasks.length === 0 && (
                <div className="empty-state">
                    <Folder size={32} />
                    <p>当前目标下暂无备份目录</p>
                </div>
            )}
            <div className="task-list">
                {tasks.map((task) => (
                    <TaskCard key={task.id} task={task} model={model} />
                ))}
            </div>
        </section>
    )
}
