import { useState } from 'react'
import { GlassButton } from './glass-controls'
import { GlassInput } from './glass-fields'
import { Dialog } from './workflow'

export function PasswordDialog({
    title,
    mode,
    onClose,
    onSubmit,
}: {
    title: string
    mode: 'create' | 'unlock' | 'import'
    onClose: () => void
    onSubmit: (password: string, allowPlain: boolean) => Promise<void>
}) {
    const [password, setPassword] = useState('')
    const [confirm, setConfirm] = useState('')
    const [allowPlain, setAllowPlain] = useState(false)
    const [pending, setPending] = useState(false)
    const [error, setError] = useState('')
    return (
        <Dialog
            title={title}
            onClose={() => {
                if (!pending) {
                    onClose()
                }
            }}
        >
            <form
                className="filter-dialog-body"
                onSubmit={async (event) => {
                    event.preventDefault()
                    if (mode === 'create' && password !== confirm) {
                        setError('两次输入的密码不一致')
                        return
                    }
                    setPending(true)
                    setError('')
                    try {
                        await onSubmit(password, allowPlain)
                        onClose()
                    } catch (failure) {
                        const message = String(failure)
                        setError(
                            message.includes('Authentication failed')
                                ? '密码错误，或加密数据已损坏'
                                : message.includes('new, empty repository') ||
                                    message.includes('no previous backups')
                                  ? '请使用没有备份历史的新仓库启用加密'
                                  : message,
                        )
                    } finally {
                        setPending(false)
                    }
                }}
            >
                {mode === 'create' && (
                    <p className="muted">
                        仅空仓库可启用。请妥善保存密码，遗失后无法恢复备份。
                    </p>
                )}
                <label>
                    {mode === 'import' ? '归档密码' : '仓库密码'}
                    <GlassInput
                        autoFocus
                        required
                        type="password"
                        autoComplete={
                            mode === 'create'
                                ? 'new-password'
                                : 'current-password'
                        }
                        disabled={pending}
                        maxLength={256}
                        value={password}
                        onChange={(e) => setPassword(e.target.value)}
                    />
                </label>
                {mode === 'create' && (
                    <label>
                        确认密码
                        <GlassInput
                            required
                            type="password"
                            autoComplete="new-password"
                            disabled={pending}
                            maxLength={256}
                            value={confirm}
                            onChange={(e) => setConfirm(e.target.value)}
                        />
                    </label>
                )}
                {mode === 'import' && (
                    <label className="realtime-toggle">
                        <input
                            type="checkbox"
                            checked={allowPlain}
                            disabled={pending}
                            onChange={(e) => setAllowPlain(e.target.checked)}
                        />
                        允许将内容导入未加密的目标
                    </label>
                )}
                {error && (
                    <p role="alert" className="error-text">
                        {error}
                    </p>
                )}
                <div className="form-actions">
                    <GlassButton
                        type="button"
                        disabled={pending}
                        onClick={onClose}
                    >
                        取消
                    </GlassButton>
                    <GlassButton
                        type="submit"
                        className="backup-button"
                        disabled={pending}
                    >
                        {
                            {
                                create: '启用加密',
                                unlock: '解锁仓库',
                                import: '选择归档',
                            }[mode]
                        }
                    </GlassButton>
                </div>
            </form>
        </Dialog>
    )
}
