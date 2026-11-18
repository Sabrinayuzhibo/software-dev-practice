import { dialog, ipcMain, type BrowserWindow } from 'electron'
import type { AgentBridge } from './agent-bridge'

function identifier(value: unknown): string {
    if (
        typeof value !== 'string' ||
        !/^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$/.test(value)
    ) {
        throw new Error('请选择有效的备份目标和版本')
    }
    return value
}

// 主进程选择归档路径，只向 Agent 传路径；文件字节不进入 Renderer。
export function registerArchiveHandlers(
    bridge: AgentBridge,
    window: () => BrowserWindow,
): void {
    ipcMain.handle(
        'archives:export',
        async (_event, target: unknown, version: unknown) => {
            const targetId = identifier(target)
            const versionId = identifier(version)
            const choice = await dialog.showSaveDialog(window(), {
                title: '将版本导出为新归档文件',
                defaultPath: `backup-${versionId}.bsar`,
                filters: [{ name: 'Backup System 归档', extensions: ['bsar'] }],
            })
            if (choice.canceled || !choice.filePath) {
                return null
            }
            return bridge.send('start_export_archive', {
                target_id: targetId,
                version_id: versionId,
                archive_path: choice.filePath,
            })
        },
    )
    ipcMain.handle(
        'archives:import',
        async (
            _event,
            target: unknown,
            password?: unknown,
            allowPlain?: unknown,
        ) => {
            const targetId = identifier(target)
            if (
                password !== undefined &&
                (typeof password !== 'string' ||
                    Buffer.byteLength(password) > 1024)
            ) {
                throw new Error('归档密码无效')
            }
            const choice = await dialog.showOpenDialog(window(), {
                title: '选择归档并导入当前备份目标',
                properties: ['openFile'],
                filters: [
                    { name: 'Backup System 归档', extensions: ['bsar'] },
                    { name: '所有文件', extensions: ['*'] },
                ],
            })
            if (choice.canceled || choice.filePaths.length !== 1) {
                return null
            }
            return bridge.send('start_import_archive', {
                target_id: targetId,
                archive_path: choice.filePaths[0],
                ...(password ? { archive_password: password } : {}),
                ...(allowPlain === true ? { allow_plain_import: true } : {}),
            })
        },
    )
}
