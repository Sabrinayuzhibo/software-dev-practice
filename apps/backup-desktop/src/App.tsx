import {
    Archive,
    Folder,
    ListChecks,
    RefreshCw,
    Settings,
    ShieldCheck,
    X,
} from 'lucide-react'
import { useEffect, useRef } from 'react'
import { TitleBar } from './ui'
import { useBackupConsole } from './use-backup-console'
import { TargetsView, TasksView, VersionsView } from './views'
import { RecordsView } from './records-view'
import { OperationPanel, WorkflowDialogs } from './workflow'

export default function App() {
    const model = useBackupConsole()
    const contentRef = useRef<HTMLElement>(null)
    const showOperationPanel =
        model.tab === 'records' ||
        (model.tab === 'versions' &&
            model.detail?.action === 'restore' &&
            model.detail.target.id === model.targetId)
    const panelId = showOperationPanel ? model.detail?.id : undefined
    useEffect(() => {
        contentRef.current?.scrollTo({ top: 0 })
    }, [model.tab, panelId])
    const {
        config,
        targetId,
        selectTarget,
        tab,
        navigate,
        connection,
        message,
        setMessage,
        busy,
        target,
        refreshConnection,
    } = model
    const tabs = [
        { id: 'tasks', title: '备份任务', icon: Folder },
        { id: 'versions', title: '备份版本', icon: Archive },
        { id: 'records', title: '执行记录', icon: ListChecks },
        { id: 'targets', title: '备份目标', icon: Settings },
    ]
    return (
        <div className="desktop">
            <TitleBar />
            <div className="app-shell">
                <aside className="sidebar">
                    <div className="sidebar-label">工作空间</div>
                    {tabs.map((item) => (
                        <button
                            key={item.id}
                            data-tab={item.id}
                            aria-current={tab === item.id ? 'page' : undefined}
                            className={`nav-item ${tab === item.id ? 'active' : ''}`}
                            onClick={() => navigate(item.id)}
                        >
                            <item.icon size={18} />
                            {item.title}
                        </button>
                    ))}
                    <div className="sidebar-footer">
                        <ShieldCheck size={17} />
                        本地仓库
                    </div>
                </aside>
                <main className="content" ref={contentRef}>
                    <header className="topbar">
                        <h1>{tabs.find((item) => item.id === tab)?.title}</h1>
                        <label className="target-selector">
                            备份目标
                            <select
                                value={targetId}
                                disabled={busy}
                                onChange={(event) =>
                                    selectTarget(event.target.value)
                                }
                            >
                                {config.targets.map((item) => (
                                    <option key={item.id} value={item.id}>
                                        {item.name}
                                    </option>
                                ))}
                            </select>
                        </label>
                    </header>
                    <section className="status-panel">
                        <span className={`status-dot ${connection.state}`} />
                        <div className="status-copy">
                            <strong>{connection.detail}</strong>
                            <span>
                                {target?.host}:{target?.port}
                                {connection.root && ` · ${connection.root}`}
                            </span>
                            {connection.error && (
                                <details>
                                    <summary>连接详情</summary>
                                    <p>{connection.error}</p>
                                </details>
                            )}
                        </div>
                        <button
                            className="text-button"
                            title="检查备份目标连接"
                            disabled={busy || connection.state === 'checking'}
                            onClick={() => void refreshConnection()}
                        >
                            <RefreshCw
                                size={16}
                                className={
                                    connection.state === 'checking'
                                        ? 'spinning'
                                        : ''
                                }
                            />
                            {connection.state === 'checking'
                                ? '检查中'
                                : '重新检查'}
                        </button>
                    </section>
                    {message && (
                        <div className="message-banner" role="alert">
                            <span>{message}</span>
                            <button
                                className="icon-button"
                                title="关闭提示"
                                aria-label="关闭提示"
                                onClick={() => setMessage('')}
                            >
                                <X size={16} />
                            </button>
                        </div>
                    )}
                    {model.preparingRestore && tab === 'versions' && (
                        <p className="pending-operation" role="status">
                            <RefreshCw className="spinning" size={16} />
                            正在准备还原
                        </p>
                    )}
                    {showOperationPanel && model.detail && (
                        <OperationPanel model={model} detail={model.detail} />
                    )}
                    {tab === 'tasks' && <TasksView model={model} />}
                    {tab === 'versions' && <VersionsView model={model} />}
                    {tab === 'records' && <RecordsView model={model} />}
                    {tab === 'targets' && <TargetsView model={model} />}
                </main>
            </div>
            <WorkflowDialogs model={model} />
        </div>
    )
}
