import type { ScanResult } from './types'

export type ScanEntry = ScanResult['sample'][number]

export interface ScanTreeNode {
    path: string
    name: string
    entry?: ScanEntry
    children: ScanTreeNode[]
}

export const isScanDirectory = (node: ScanTreeNode): boolean =>
    node.entry?.type === 'directory' || !node.entry

/** 从扫描样本建立层级；缺少的父目录仅用于展示，不增加扫描统计。 */
export function buildScanTree(entries: ScanEntry[]): ScanTreeNode[] {
    const nodes = new Map<string, ScanTreeNode>()
    const roots: ScanTreeNode[] = []
    for (const entry of entries) {
        let siblings = roots
        let path = ''
        for (const name of entry.path.split('/')) {
            path = path ? `${path}/${name}` : name
            let node = nodes.get(path)
            if (!node) {
                node = { path, name, children: [] }
                nodes.set(path, node)
                siblings.push(node)
            }
            siblings = node.children
        }
        const node = nodes.get(entry.path)
        if (node) {
            node.entry = entry
        }
    }
    const compare = new Intl.Collator('zh-CN', { numeric: true })
    for (const siblings of [
        roots,
        ...Array.from(nodes.values(), (node) => node.children),
    ]) {
        siblings.sort(
            (left, right) =>
                Number(isScanDirectory(right)) -
                    Number(isScanDirectory(left)) ||
                compare.compare(left.name, right.name),
        )
    }
    return roots
}

/** 迭代遍历避免深层目录递归；省略 expanded 时返回全部层级。 */
export function visibleScanRows(roots: ScanTreeNode[], expanded?: Set<string>) {
    const pending = roots.map((node) => ({ node, depth: 0 })).reverse()
    const rows: { node: ScanTreeNode; depth: number }[] = []
    while (pending.length) {
        const current = pending.pop()!
        rows.push(current)
        if (
            isScanDirectory(current.node) &&
            (!expanded || expanded.has(current.node.path))
        ) {
            for (
                let index = current.node.children.length - 1;
                index >= 0;
                --index
            ) {
                pending.push({
                    node: current.node.children[index],
                    depth: current.depth + 1,
                })
            }
        }
    }
    return rows
}
