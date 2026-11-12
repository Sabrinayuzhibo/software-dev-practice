import { useState } from 'react'
import { Plus, X } from 'lucide-react'
import type { BackupFilters, FilterCategory, FilterRule } from './types'

export const emptyFilters = (): BackupFilters => ({ include: [], exclude: [] })
const categories: Record<FilterCategory, string> = {
    path: '路径',
    type: '类型',
    name: '名字',
    mtime: '时间',
    size: '尺寸',
    user: '用户',
}
const types: Record<string, string> = {
    file: '普通文件',
    directory: '目录',
    symlink: '软链接',
    hardlink: '硬链接成员',
    fifo: '命名管道',
    character_device: '字符设备',
    block_device: '块设备',
}

function ruleError(rule: FilterRule): string {
    if (rule.category === 'name' || rule.category === 'path') {
        const pattern = rule.pattern || ''
        if (
            !pattern ||
            pattern.length > 512 ||
            pattern.includes('\0') ||
            pattern.startsWith('/')
        ) {
            return rule.category === 'name'
                ? '请填写名字条件，例如 *.txt；最多 512 个字符，不能包含空字符或以 / 开头。'
                : '请填写相对路径，例如 docs/；最多 512 个字符，不能包含空字符或以 / 开头。'
        }
        if (rule.category === 'name' && pattern.includes('/')) {
            return '名字不能包含路径分隔符。'
        }
        if (
            rule.category === 'path' &&
            pattern
                .replace(/\/$/, '')
                .split('/')
                .some((part) => !part || part === '.' || part === '..')
        ) {
            return '路径不能包含空段、. 或 ..。'
        }
    } else if (rule.category === 'user') {
        if (
            !/^\d+$/.test(rule.value || '') ||
            BigInt(rule.value!) > 4294967295n
        ) {
            return 'UID 必须是 0 至 4294967295 的整数。'
        }
    } else if (rule.category === 'size' || rule.category === 'mtime') {
        if (rule.min === undefined && rule.max === undefined) {
            return '范围至少填写一个边界。'
        }
        for (const value of [rule.min, rule.max]) {
            if (value === undefined) {
                continue
            }
            if (!/^-?\d+$/.test(value)) {
                return '范围边界必须是整数。'
            }
            const number = BigInt(value)
            if (
                number <
                    (rule.category === 'size' ? 0n : -9223372036854775808n) ||
                number > 9223372036854775807n
            ) {
                return '范围超出有效值，尺寸不能为负。'
            }
        }
        if (
            rule.min !== undefined &&
            rule.max !== undefined &&
            BigInt(rule.min) > BigInt(rule.max)
        ) {
            return '下限不能大于上限。'
        }
    }
    return ''
}

interface FilterIssue {
    message: string
    category?: FilterCategory
}

// 保留错误所在类别和组内序号，避免跨页签时把错误归到当前类别。
function filterIssue(filters: BackupFilters): FilterIssue | null {
    for (const polarity of ['include', 'exclude'] as const) {
        if (filters[polarity].length > 100) {
            return { message: '选入和排除各最多 100 条规则。' }
        }
        const counts: Partial<Record<FilterCategory, number>> = {}
        for (const rule of filters[polarity]) {
            const number = (counts[rule.category] || 0) + 1
            counts[rule.category] = number
            const message = ruleError(rule)
            if (message) {
                const group = polarity === 'include' ? '只备份这些' : '跳过这些'
                return {
                    category: rule.category,
                    message: `${categories[rule.category]} · ${group} · 第 ${number} 条：${message}`,
                }
            }
        }
    }
    if (new TextEncoder().encode(JSON.stringify(filters)).length > 16 * 1024) {
        return { message: '规则总长度不能超过 16 KiB。' }
    }
    return null
}

export function filterError(filters: BackupFilters): string {
    return filterIssue(filters)?.message || ''
}

function localTime(seconds: string | undefined): string {
    if (seconds === undefined) {
        return ''
    }
    const date = new Date(Number(seconds) * 1000)
    if (!Number.isFinite(date.getTime())) {
        return ''
    }
    return new Date(date.getTime() - date.getTimezoneOffset() * 60000)
        .toISOString()
        .slice(0, 19)
}

function ruleDescription(rule: FilterRule): string {
    if (rule.category === 'path' || rule.category === 'name') {
        return rule.pattern || '未填写'
    }
    if (rule.category === 'type') {
        return types[rule.value || ''] || rule.value || ''
    }
    if (rule.category === 'user') {
        return `UID ${rule.value}`
    }
    const show = (value?: string) =>
        value === undefined
            ? '不限'
            : rule.category === 'mtime'
              ? new Date(Number(value) * 1000).toLocaleString()
              : `${value} 字节`
    return `${show(rule.min)} ～ ${show(rule.max)}`
}

export function FilterSummary({ filters }: { filters?: BackupFilters }) {
    if (!filters || (!filters.include.length && !filters.exclude.length)) {
        return null
    }
    return (
        <details className="filter-summary">
            <summary>
                自定义筛选：选入 {filters.include.length} 条，排除{' '}
                {filters.exclude.length} 条
            </summary>
            {(['include', 'exclude'] as const).map((polarity) =>
                filters[polarity].map((rule, index) => (
                    <small key={`${polarity}-${index}`}>
                        {polarity === 'include' ? '选入' : '排除'} ·{' '}
                        {categories[rule.category]}：{ruleDescription(rule)}
                    </small>
                )),
            )}
        </details>
    )
}

function RuleInput({
    rule,
    onChange,
}: {
    rule: FilterRule
    onChange: (rule: FilterRule) => void
}) {
    const category = rule.category
    if (category === 'type') {
        return (
            <select
                aria-label="匹配类型"
                value={rule.value}
                onChange={(event) =>
                    onChange({ ...rule, value: event.target.value })
                }
            >
                {Object.entries(types).map(([type, label]) => (
                    <option key={type} value={type}>
                        {label}
                    </option>
                ))}
            </select>
        )
    }
    if (category === 'size' || category === 'mtime') {
        const setBoundary = (bound: 'min' | 'max', value: string) => {
            const next = { ...rule }
            if (!value) {
                delete next[bound]
            } else {
                next[bound] =
                    category === 'mtime'
                        ? String(Math.floor(new Date(value).getTime() / 1000))
                        : value
            }
            onChange(next)
        }
        return (
            <>
                {(['min', 'max'] as const).map((bound) => (
                    <label key={bound}>
                        {category === 'mtime'
                            ? bound === 'min'
                                ? '开始时间'
                                : '结束时间'
                            : bound === 'min'
                              ? '最小值（字节）'
                              : '最大值（字节）'}
                        <input
                            aria-label={`${categories[category]}${bound === 'min' ? '下限' : '上限'}`}
                            type={
                                category === 'mtime' ? 'datetime-local' : 'text'
                            }
                            step={1}
                            inputMode={
                                category === 'size' ? 'numeric' : undefined
                            }
                            placeholder="不限"
                            value={
                                category === 'mtime'
                                    ? localTime(rule[bound])
                                    : rule[bound] || ''
                            }
                            onChange={(event) =>
                                setBoundary(bound, event.target.value)
                            }
                        />
                    </label>
                ))}
            </>
        )
    }
    return (
        <input
            aria-label={`${categories[category]}条件`}
            value={category === 'user' ? rule.value || '' : rule.pattern || ''}
            placeholder={
                category === 'path'
                    ? '例如 sub/'
                    : category === 'name'
                      ? '例如 *.txt'
                      : '例如 1000'
            }
            onChange={(event) =>
                onChange({
                    ...rule,
                    [category === 'user' ? 'value' : 'pattern']:
                        event.target.value,
                })
            }
        />
    )
}

function FilterTabs({
    category,
    value,
    onSelect,
}: {
    category: FilterCategory
    value: BackupFilters
    onSelect: (category: FilterCategory) => void
}) {
    return (
        <div className="filter-tabs" role="tablist" aria-label="筛选类别">
            {Object.entries(categories).map(([key, label]) => {
                const count = [...value.include, ...value.exclude].filter(
                    (rule) => rule.category === key,
                ).length
                return (
                    <button
                        type="button"
                        key={key}
                        role="tab"
                        tabIndex={category === key ? 0 : -1}
                        aria-selected={category === key}
                        aria-controls={`filter-panel-${key}`}
                        id={`filter-tab-${key}`}
                        onClick={() => onSelect(key as FilterCategory)}
                        onKeyDown={(event) => {
                            const keys = Object.keys(
                                categories,
                            ) as FilterCategory[]
                            const index = keys.indexOf(category)
                            let next = index
                            if (event.key === 'ArrowRight') {
                                next = (index + 1) % keys.length
                            } else if (event.key === 'ArrowLeft') {
                                next = (index + keys.length - 1) % keys.length
                            } else if (event.key === 'Home') {
                                next = 0
                            } else if (event.key === 'End') {
                                next = keys.length - 1
                            } else {
                                return
                            }
                            event.preventDefault()
                            onSelect(keys[next])
                            event.currentTarget.parentElement
                                ?.querySelector<HTMLButtonElement>(
                                    `#filter-tab-${keys[next]}`,
                                )
                                ?.focus()
                        }}
                    >
                        {label}
                        {count > 0 && (
                            <span className="filter-tab-count">{count}</span>
                        )}
                    </button>
                )
            })}
        </div>
    )
}

export function FilterEditor({
    value,
    onChange,
    disabled = false,
}: {
    value: BackupFilters
    onChange: (filters: BackupFilters) => void
    disabled?: boolean
}) {
    const [category, setCategory] = useState<FilterCategory>('path')
    const issue = filterIssue(value)
    const update = (
        polarity: 'include' | 'exclude',
        index: number,
        rule: FilterRule,
    ) => {
        onChange({
            ...value,
            [polarity]: value[polarity].map((old, position) =>
                position === index ? rule : old,
            ),
        })
    }
    return (
        <section className="filter-editor" aria-label="自定义备份筛选">
            <div className="filter-editor-heading">
                <h3>自定义备份</h3>
                <span className="filter-count">
                    {value.include.length + value.exclude.length
                        ? `已添加 ${value.include.length + value.exclude.length} 条条件`
                        : '可选设置'}
                </span>
            </div>
            <p className="filter-intro">不设置条件时，按上方文件类型备份。</p>
            <FilterTabs
                category={category}
                value={value}
                onSelect={setCategory}
            />
            <div
                role="tabpanel"
                id={`filter-panel-${category}`}
                aria-labelledby={`filter-tab-${category}`}
            >
                <p className="filter-help">
                    {category === 'path'
                        ? '例如 docs/：匹配该文件夹及其中的文件。请填写相对备份根目录的路径。'
                        : category === 'name'
                          ? '例如 *.txt：匹配所有 .txt 文件；输入完整文件名可精确匹配。'
                          : category === 'size'
                            ? '按文件大小筛选，单位为字节。不填最小值或最大值，表示该端不限。'
                            : category === 'mtime'
                              ? '按文件的最后修改时间筛选，使用本地时间。可只填开始或结束时间。'
                              : category === 'user'
                                ? '按文件属主筛选。填写用户的数字 UID，例如 1000。'
                                : '在上方已启用的文件类型中进一步筛选。'}
                </p>
                {(['include', 'exclude'] as const).map((polarity) => (
                    <fieldset
                        key={polarity}
                        disabled={disabled}
                        aria-label={
                            polarity === 'include' ? '只备份这些' : '跳过这些'
                        }
                    >
                        <div className="filter-group-heading">
                            <h4>
                                {polarity === 'include'
                                    ? '只备份这些'
                                    : '跳过这些'}
                            </h4>
                            <button
                                type="button"
                                className={`add-filter-${polarity}`}
                                disabled={value[polarity].length >= 100}
                                onClick={() =>
                                    onChange({
                                        ...value,
                                        [polarity]: [
                                            ...value[polarity],
                                            {
                                                category,
                                                ...(category === 'type'
                                                    ? { value: 'file' }
                                                    : {}),
                                            },
                                        ],
                                    })
                                }
                            >
                                <Plus size={14} aria-hidden="true" />
                                添加条件
                            </button>
                        </div>
                        {!value[polarity].some(
                            (rule) => rule.category === category,
                        ) && (
                            <p className="filter-empty">
                                {polarity === 'include'
                                    ? `未限制${categories[category]}，全部保留`
                                    : '未设置排除条件'}
                            </p>
                        )}
                        {value[polarity].map((rule, index) =>
                            rule.category !== category ? null : (
                                <div
                                    className="filter-rule"
                                    key={index}
                                    data-polarity={polarity}
                                >
                                    <RuleInput
                                        rule={rule}
                                        onChange={(next) =>
                                            update(polarity, index, next)
                                        }
                                    />
                                    <button
                                        type="button"
                                        className="remove-filter"
                                        aria-label="删除筛选条件"
                                        title="删除此条件"
                                        onClick={() =>
                                            onChange({
                                                ...value,
                                                [polarity]: value[
                                                    polarity
                                                ].filter(
                                                    (_, position) =>
                                                        position !== index,
                                                ),
                                            })
                                        }
                                    >
                                        <X size={16} aria-hidden="true" />
                                    </button>
                                </div>
                            ),
                        )}
                    </fieldset>
                ))}
            </div>
            {issue && (
                <div className="filter-error" role="alert">
                    <p className="error-text">{issue.message}</p>
                    {issue.category && (
                        <button
                            type="button"
                            className="locate-filter-error"
                            onClick={(event) => {
                                setCategory(issue.category!)
                                event.currentTarget
                                    .closest('.filter-editor')
                                    ?.querySelector<HTMLButtonElement>(
                                        `#filter-tab-${issue.category}`,
                                    )
                                    ?.focus()
                            }}
                        >
                            定位条件
                        </button>
                    )}
                </div>
            )}
            <details className="filter-guidance">
                <summary>筛选规则说明</summary>
                <p>
                    同类“只备份”条件满足任意一条即可，不同类别需要同时满足。“跳过”条件优先，匹配后不会备份；跳过文件夹时也会跳过其中的文件。
                </p>
                <p>
                    路径和名字支持 *、?
                    通配符，并区分大小写；路径通配符不跨目录。时间和大小包含两端边界。目录不按大小选入，软链接和管道不匹配大小选入条件。硬链接成员指源文件链接数大于
                    1 的普通文件。
                </p>
            </details>
            <FilterSummary filters={value} />
        </section>
    )
}
