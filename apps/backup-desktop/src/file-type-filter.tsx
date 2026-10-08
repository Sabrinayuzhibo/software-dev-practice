import { ChevronDown, ChevronUp } from 'lucide-react'
import { useState } from 'react'
import type { ContentFileType, FileType } from './types'

export const defaultFileTypes: ContentFileType[] = ['file', 'symlink', 'fifo']
const specialFileTypes: ContentFileType[] = [
    'character_device',
    'block_device',
    'socket',
]
export const supportedFileTypes: ContentFileType[] = [
    ...defaultFileTypes,
    ...specialFileTypes,
]

const labels: Record<FileType, string> = {
    file: '普通文件',
    directory: '目录',
    symlink: '软链接',
    fifo: '命名管道',
    character_device: '字符设备',
    block_device: '块设备',
    socket: '套接字',
}

const hints: Partial<Record<ContentFileType, string>> = {
    file: '包含硬链接成员',
    character_device: '只保存设备类型和主/次设备号；还原需要创建节点权限',
    block_device:
        '只保存设备类型和主/次设备号，不读取磁盘内容；还原需要创建节点权限',
    socket: '只保存路径节点，不保存连接和服务状态',
}

export function FileTypePicker({
    selected,
    preserveEmptyDirs,
    disabled,
    onChange,
    onEmptyDirectoryChange,
}: {
    selected: ContentFileType[]
    preserveEmptyDirs: boolean
    disabled: boolean
    onChange: (types: ContentFileType[]) => void
    onEmptyDirectoryChange: (preserve: boolean) => void
}) {
    const [specialExpanded, setSpecialExpanded] = useState(false)
    const option = (type: ContentFileType) => (
        <label key={type} data-file-type={type} title={hints[type]}>
            <input
                type="checkbox"
                checked={selected.includes(type)}
                onChange={(event) =>
                    onChange(
                        supportedFileTypes.filter((item) =>
                            item === type
                                ? event.target.checked
                                : selected.includes(item),
                        ),
                    )
                }
            />
            {labels[type]}
        </label>
    )
    return (
        <fieldset className="file-type-picker" disabled={disabled}>
            <legend>文件类型</legend>
            <div className="file-type-options">
                {defaultFileTypes.map(option)}
                <label className="all-file-types">
                    <input
                        type="checkbox"
                        checked={defaultFileTypes.every((type) =>
                            selected.includes(type),
                        )}
                        onChange={(event) =>
                            onChange(
                                supportedFileTypes.filter((type) =>
                                    specialFileTypes.includes(type)
                                        ? selected.includes(type)
                                        : event.target.checked,
                                ),
                            )
                        }
                    />
                    常用全选
                </label>
            </div>
            <div className="file-type-secondary">
                <label className="empty-directory-option">
                    <input
                        type="checkbox"
                        checked={preserveEmptyDirs}
                        onChange={(event) =>
                            onEmptyDirectoryChange(event.target.checked)
                        }
                    />
                    保留空目录
                </label>
                <button
                    type="button"
                    className="special-type-toggle"
                    aria-expanded={specialExpanded}
                    onClick={() => setSpecialExpanded(!specialExpanded)}
                >
                    {specialExpanded ? (
                        <ChevronUp size={15} />
                    ) : (
                        <ChevronDown size={15} />
                    )}
                    设备与套接字
                </button>
            </div>
            {specialExpanded && (
                <div className="file-type-options special-file-types">
                    {specialFileTypes.map(option)}
                </div>
            )}
        </fieldset>
    )
}

export function FileTypeSummary({
    types,
    preserveEmptyDirs,
}: {
    types?: FileType[]
    preserveEmptyDirs?: boolean
}) {
    if (!types?.length && preserveEmptyDirs === undefined) {
        return null
    }
    return (
        <small className="file-type-summary">
            类型：
            {(
                [
                    'file',
                    'directory',
                    'symlink',
                    'fifo',
                    'character_device',
                    'block_device',
                    'socket',
                ] as FileType[]
            )
                .filter((type) => types?.includes(type))
                .map((type) => labels[type])
                .join('、')}
            {preserveEmptyDirs === undefined
                ? ''
                : ` · ${preserveEmptyDirs ? '保留空目录' : '不保留空目录'}`}
        </small>
    )
}
