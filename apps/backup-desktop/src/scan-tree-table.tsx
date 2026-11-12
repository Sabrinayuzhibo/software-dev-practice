import {
    ChevronDown,
    ChevronRight,
    File,
    Folder,
    FolderOpen,
} from 'lucide-react'
import { useMemo, useState } from 'react'
import type { ScanResult } from './types'
import { buildScanTree, isScanDirectory, visibleScanRows } from './scan-tree'

/** 目录树只改变样本展示，不读取链接目标或改变备份范围。 */
export function ScanTreeTable({
    result,
    typeLabels,
    formatSize,
}: {
    result: ScanResult
    typeLabels: Record<string, string>
    formatSize: (size: number) => string
}) {
    const roots = useMemo(() => buildScanTree(result.sample), [result.sample])
    const [expanded, setExpanded] = useState(
        () => new Set(roots.filter(isScanDirectory).map((node) => node.path)),
    )
    const rows = visibleScanRows(roots, expanded)
    const directories = useMemo(
        () =>
            visibleScanRows(roots)
                .filter(({ node }) => isScanDirectory(node))
                .map(({ node }) => node.path),
        [roots],
    )
    function toggle(path: string) {
        setExpanded((previous) => {
            const next = new Set(previous)
            if (next.has(path)) {
                next.delete(path)
            } else {
                next.add(path)
            }
            return next
        })
    }
    return (
        <>
            <div className="scan-tree-toolbar">
                <span>点击目录箭头查看下一级</span>
                <div>
                    <button
                        type="button"
                        className="scan-expand-all"
                        disabled={!directories.length}
                        onClick={() => setExpanded(new Set(directories))}
                    >
                        全部展开
                    </button>
                    <button
                        type="button"
                        className="scan-collapse-all"
                        disabled={!expanded.size}
                        onClick={() => setExpanded(new Set())}
                    >
                        全部收起
                    </button>
                </div>
            </div>
            <div className="table-scroll">
                <table
                    className="scan-table scan-tree-table"
                    aria-label="扫描文件目录树"
                >
                    <thead>
                        <tr>
                            <th>名称</th>
                            <th>类型</th>
                            <th>大小</th>
                            <th>内容校验值</th>
                        </tr>
                    </thead>
                    <tbody>
                        {rows.map(({ node, depth }) => {
                            const file = node.entry
                            const directory = isScanDirectory(node)
                            const open = directory && expanded.has(node.path)
                            const Icon = directory
                                ? open
                                    ? FolderOpen
                                    : Folder
                                : File
                            return (
                                <tr
                                    key={node.path}
                                    data-path={node.path}
                                    data-type={
                                        file?.type ||
                                        (directory ? 'directory' : 'file')
                                    }
                                >
                                    <td>
                                        <div
                                            className="scan-tree-name"
                                            style={{
                                                paddingInlineStart:
                                                    Math.min(depth, 10) * 16,
                                            }}
                                        >
                                            {directory ? (
                                                <button
                                                    type="button"
                                                    className="scan-directory-toggle"
                                                    aria-label={`${open ? '收起' : '展开'}目录 ${node.path}`}
                                                    aria-expanded={open}
                                                    onClick={() =>
                                                        toggle(node.path)
                                                    }
                                                >
                                                    {open ? (
                                                        <ChevronDown
                                                            size={14}
                                                        />
                                                    ) : (
                                                        <ChevronRight
                                                            size={14}
                                                        />
                                                    )}
                                                </button>
                                            ) : (
                                                <span className="scan-tree-spacer" />
                                            )}
                                            <Icon
                                                size={16}
                                                className={
                                                    directory
                                                        ? 'folder-icon'
                                                        : ''
                                                }
                                                aria-hidden="true"
                                            />
                                            <span title={node.path}>
                                                {node.name}
                                            </span>
                                        </div>
                                        {open && !node.children.length && (
                                            <small>此目录没有预览子项</small>
                                        )}
                                        {!file && <small>路径层级</small>}
                                        {file?.link_target !== undefined && (
                                            <small>
                                                指向：{file.link_target}
                                            </small>
                                        )}
                                        {file?.link_to && (
                                            <small>
                                                与此文件共享数据：{file.link_to}
                                            </small>
                                        )}
                                        {file?.device_major !== undefined &&
                                            file.device_minor !== undefined && (
                                                <small>
                                                    设备号：{file.device_major}:
                                                    {file.device_minor}
                                                </small>
                                            )}
                                    </td>
                                    <td>
                                        {typeLabels[
                                            file?.type ||
                                                (directory
                                                    ? 'directory'
                                                    : 'file')
                                        ] || '未知类型'}
                                    </td>
                                    <td>
                                        {file?.size === undefined
                                            ? '-'
                                            : formatSize(file.size)}
                                    </td>
                                    <td className="digest">
                                        {file?.sha256 ? (
                                            <>
                                                <span
                                                    className="digest-short"
                                                    title={`SHA-256: ${file.sha256}`}
                                                >
                                                    {file.sha256}
                                                </span>
                                                <details className="digest-details">
                                                    <summary>SHA-256</summary>
                                                    <code>{file.sha256}</code>
                                                </details>
                                            </>
                                        ) : (
                                            '-'
                                        )}
                                    </td>
                                </tr>
                            )
                        })}
                    </tbody>
                </table>
            </div>
            <p className="muted scan-tree-note">
                共 {result.sample.length}{' '}
                个预览条目；目录树仅展示预览内容，实际备份范围以来源和筛选规则为准。
            </p>
        </>
    )
}
