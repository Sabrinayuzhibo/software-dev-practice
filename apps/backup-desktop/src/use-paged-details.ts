import { useEffect, useRef, useState } from 'react'

interface PageCursor {
    next_offset: number | null
}

/** 共用惰性分页和异步生命周期；资源变化时调用组件须用 key 重新挂载。 */
export function usePagedDetails<Item, Page extends PageCursor>(
    request: (offset: number) => Promise<Page>,
    itemsOf: (page: Page) => Item[],
) {
    const [items, setItems] = useState<Item[]>([])
    const [page, setPage] = useState<Page | null>(null)
    const [loading, setLoading] = useState(false)
    const [error, setError] = useState('')
    const pending = useRef(false)
    const mounted = useRef(true)
    const nextOffset = page ? page.next_offset : 0
    useEffect(() => {
        mounted.current = true
        return () => {
            mounted.current = false
        }
    }, [])

    async function load() {
        if (pending.current || nextOffset === null) {
            return
        }
        pending.current = true
        setLoading(true)
        setError('')
        try {
            const next = await request(nextOffset)
            if (mounted.current) {
                setItems((old) =>
                    nextOffset ? [...old, ...itemsOf(next)] : itemsOf(next),
                )
                setPage(next)
            }
        } catch (failure) {
            if (mounted.current) {
                setError(String(failure))
            }
        } finally {
            pending.current = false
            if (mounted.current) {
                setLoading(false)
            }
        }
    }
    return { items, page, loading, error, nextOffset, load }
}
