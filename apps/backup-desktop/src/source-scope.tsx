import type { BackupTask, SourceEntry } from './types'
import { entryTypeLabels } from './ui'

export function sourcePath(root: string, relative: string): string {
    return `${root === '/' ? '' : root}/${relative}`
}

export function taskTitle(
    task: Pick<BackupTask, 'path' | 'name' | 'selection'>,
): string {
    if (task.name) {
        return task.name
    }
    const first = task.selection?.[0]?.path || task.path
    const name = first.split('/').pop() || first
    return task.selection && task.selection.length > 1
        ? `${name} 等 ${task.selection.length} 项`
        : name
}

export function sourceLabel(root: string, selection?: SourceEntry[]): string {
    return selection?.length === 1 ? sourcePath(root, selection[0].path) : root
}

export function SourceDetails({
    root,
    selection,
}: {
    root: string
    selection?: SourceEntry[]
}) {
    if (!selection) {
        return null
    }
    return (
        <details className="source-details">
            <summary>已选 {selection.length} 项</summary>
            <ul>
                {selection.map((item) => (
                    <li key={item.path}>
                        <span className="path">
                            {sourcePath(root, item.path)}
                        </span>
                        <small>
                            {entryTypeLabels[item.type]} · 还原路径：{item.path}
                        </small>
                    </li>
                ))}
            </ul>
        </details>
    )
}
