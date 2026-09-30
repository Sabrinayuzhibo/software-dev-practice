import { useEffect, useState } from 'react'
import type { BackupTask, ScanResult } from './types'

type Connection = { state: 'checking' | 'online' | 'offline'; detail: string }

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error)
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 ** 2).toFixed(1)} MB`
  return `${(bytes / 1024 ** 3).toFixed(1)} GB`
}

function TitleBar() {
  const [maximized, setMaximized] = useState(false)

  useEffect(() => {
    const unsubscribe = window.backup.onWindowMaximizedChange(setMaximized)
    void window.backup.isWindowMaximized().then(setMaximized)
    return unsubscribe
  }, [])

  return (
    <div className="window-titlebar">
      <div className="titlebar-brand">
        <span className="titlebar-logo">B</span>
        <span>Backup System</span>
      </div>
      <div className="titlebar-context">
        <span className="titlebar-separator" />
        <span>LOCAL WORKSPACE</span>
        <span className="titlebar-context-name">备份控制台</span>
      </div>
      <div className="window-controls" role="group" aria-label="窗口控制">
        <button className="window-control" type="button" title="最小化" aria-label="最小化"
          onClick={() => void window.backup.minimizeWindow()}>
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M5 17h14" /></svg>
        </button>
        <button className="window-control" type="button" title={maximized ? '还原' : '最大化'}
          aria-label={maximized ? '还原' : '最大化'}
          onClick={() => void window.backup.toggleMaximizeWindow()}>
          {maximized
            ? <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M8 5h11v11M5 8h11v11H5z" /></svg>
            : <svg viewBox="0 0 24 24" aria-hidden="true"><rect x="5" y="5" width="14" height="14" rx="2" /></svg>}
        </button>
        <button className="window-control close" type="button" title="关闭" aria-label="关闭"
          onClick={() => void window.backup.closeWindow()}>
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18" /></svg>
        </button>
      </div>
    </div>
  )
}

export default function App() {
  const [tasks, setTasks] = useState<BackupTask[]>([])
  const [connection, setConnection] = useState<Connection>({
    state: 'checking', detail: '正在连接 Backup Server…',
  })
  const [selectedId, setSelectedId] = useState<string | null>(null)
  const [scan, setScan] = useState<ScanResult | null>(null)
  const [scanning, setScanning] = useState(false)
  const [message, setMessage] = useState('')

  async function refreshConnection() {
    setConnection({ state: 'checking', detail: '正在连接 Backup Server…' })
    try {
      const result = await window.backup.pingServer()
      setConnection(result.connected
        ? { state: 'online', detail: `已连接：${result.server}` }
        : { state: 'offline', detail: `连接失败：${result.error}` })
    } catch (error) {
      setConnection({ state: 'offline', detail: `连接失败：${errorMessage(error)}` })
    }
  }

  useEffect(() => {
    void window.backup.listTasks().then(setTasks).catch((error) => {
      setMessage(`读取任务失败：${errorMessage(error)}`)
    })
    void refreshConnection()
  }, [])

  async function addFolder() {
    try {
      const task = await window.backup.addFolder()
      if (!task) return
      setTasks(await window.backup.listTasks())
      setSelectedId(task.id)
      setScan(null)
      setMessage('')
    } catch (error) {
      setMessage(`添加目录失败：${errorMessage(error)}`)
    }
  }

  async function removeTask(id: string) {
    try {
      await window.backup.removeTask(id)
      setTasks(await window.backup.listTasks())
      if (selectedId === id) {
        setSelectedId(null)
        setScan(null)
      }
      setMessage('')
    } catch (error) {
      setMessage(`移除任务失败：${errorMessage(error)}`)
    }
  }

  async function scanTask(id: string) {
    setSelectedId(id)
    setScan(null)
    setScanning(true)
    setMessage('')
    try {
      setScan(await window.backup.scanTask(id))
    } catch (error) {
      setMessage(`扫描失败：${errorMessage(error)}`)
    } finally {
      setScanning(false)
    }
  }

  const selected = tasks.find((task) => task.id === selectedId)

  return (
    <div className="desktop">
      <TitleBar />
      <div className="app-shell">
      <aside className="sidebar">
        <div className="sidebar-group">工作空间</div>
        <div className="nav-item active"><span className="nav-icon">▦</span>概览</div>
        <a className="nav-item" href="#tasks"><span className="nav-icon">▤</span>目录任务</a>
        <div className="sidebar-spacer" />
        <div className="sidebar-note">
          <strong>开发阶段</strong>
          <span>当前可检查服务端并扫描本地目录。备份上传与还原尚未开放。</span>
        </div>
      </aside>

      <main className="content">
        <header className="topbar">
          <div><div className="eyebrow">BACKUP AGENT</div><h1>备份控制台</h1></div>
          <span className="stage-badge">项目骨架 · 扫描阶段</span>
        </header>

        <section className="hero">
          <div>
            <div className="hero-kicker">本机服务 · 127.0.0.1:9000</div>
            <h2>让重要文件有迹可循</h2>
            <p>添加本地目录，使用 C++ Backup Agent 扫描文件并计算 SHA-256。服务端独立运行，后续负责保存备份版本。</p>
            <button className="primary-button" onClick={() => void addFolder()}>＋ 添加目录</button>
          </div>
          <div className="hero-symbol" aria-hidden="true">⇧</div>
        </section>

        <section className="status-panel">
          <div className={`status-dot ${connection.state}`} />
          <div className="status-copy">
            <strong>Backup Server</strong>
            <span>{connection.detail}</span>
          </div>
          <button className="text-button" onClick={() => void refreshConnection()}>重新检查</button>
        </section>

        {message && <div className="error-banner" role="alert">{message}</div>}

        <section className="task-section" id="tasks">
          <div className="section-heading">
            <div><h2>目录任务</h2><p>任务记录在本机；扫描不会上传或保存备份文件。</p></div>
            <span className="count-pill">{tasks.length} 个目录</span>
          </div>
          {tasks.length === 0 ? (
            <div className="empty-state">
              <div className="empty-icon">▧</div>
              <h3>还没有添加目录</h3>
              <p>选择一个本地目录，开始查看文件数量、容量和哈希结果。</p>
              <button className="secondary-button" onClick={() => void addFolder()}>选择目录</button>
            </div>
          ) : (
            <div className="task-list">
              {tasks.map((task) => (
                <article className={`task-card ${selectedId === task.id ? 'selected' : ''}`} key={task.id}>
                  <div className="folder-icon">▣</div>
                  <div className="task-copy">
                    <strong>{task.path.split(/[\\/]/).filter(Boolean).pop() || task.path}</strong>
                    <span title={task.path}>{task.path}</span>
                  </div>
                  <button className="secondary-button small" disabled={scanning} onClick={() => void scanTask(task.id)}>
                    {scanning && selectedId === task.id ? '扫描中…' : '扫描目录'}
                  </button>
                  <button className="icon-button" title="移除任务" aria-label="移除任务" onClick={() => void removeTask(task.id)}>×</button>
                </article>
              ))}
            </div>
          )}
        </section>

        {scan && selected && (
          <section className="result-section">
            <div className="section-heading"><div><h2>扫描结果</h2><p>{selected.path}</p></div><span className="count-pill">未执行备份</span></div>
            <div className="metrics">
              <div><span>文件</span><strong>{scan.files.toLocaleString()}</strong></div>
              <div><span>目录</span><strong>{scan.directories.toLocaleString()}</strong></div>
              <div><span>总大小</span><strong>{formatBytes(scan.bytes)}</strong></div>
              <div><span>已计算哈希</span><strong>{scan.hashed_files.toLocaleString()}</strong></div>
            </div>
            {scan.unreadable_files > 0 && <p className="warning">{scan.unreadable_files} 个文件无法读取或计算哈希。</p>}
            {scan.sample.length > 0 && (
              <div className="file-list">
                <div className="file-list-title">文件预览 <span>最多显示 100 个</span></div>
                {scan.sample.map((file) => (
                  <div className="file-row" key={file.path}>
                    <span title={file.path}>{file.path}</span>
                    <span>{formatBytes(file.size)}</span>
                    <code title={file.sha256}>{file.sha256 ? file.sha256.slice(0, 12) + '…' : '未读取'}</code>
                  </div>
                ))}
              </div>
            )}
          </section>
        )}
      </main>
      </div>
    </div>
  )
}
