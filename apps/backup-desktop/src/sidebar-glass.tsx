import { useEffect, useRef, useState } from 'react'
import { createGlassController, type GlassController } from './glass-controller'

interface GlassRect {
    x: number
    y: number
    width: number
    height: number
}

const kRecoveryDelayMilliseconds = 1000
const kMaximumRecoveryAttempts = 4

function selectedRect(sidebar: HTMLElement): GlassRect | null {
    const item = sidebar.querySelector<HTMLElement>('.nav-item.active')
    if (!item) {
        return null
    }
    const outer = sidebar.getBoundingClientRect()
    const inner = item.getBoundingClientRect()
    return {
        x: inner.left - outer.left,
        y: inner.top - outer.top,
        width: inner.width,
        height: inner.height,
    }
}

export function SidebarGlass({ activeTab }: { activeTab: string }) {
    const canvasRef = useRef<HTMLCanvasElement>(null)
    const controllerRef = useRef<GlassController | null>(null)
    const recoveryAttemptsRef = useRef(0)
    const [available, setAvailable] = useState(false)
    const [contextGeneration, setContextGeneration] = useState(0)
    const [fallbackRect, setFallbackRect] = useState<GlassRect | null>(null)

    useEffect(() => {
        const sidebar = canvasRef.current?.parentElement
        if (!sidebar) {
            return
        }
        const update = () => setFallbackRect(selectedRect(sidebar))
        const observer = new ResizeObserver(update)
        observer.observe(sidebar)
        update()
        return () => observer.disconnect()
    }, [activeTab])

    useEffect(() => {
        const canvas = canvasRef.current
        if (!canvas) {
            return
        }
        let stopped = false
        let recoveryTimer: number | undefined
        const cancelRecovery = () => {
            window.clearTimeout(recoveryTimer)
            recoveryTimer = undefined
        }
        const disposeController = () => {
            controllerRef.current?.dispose()
            controllerRef.current = null
        }
        const scheduleRecovery = (error?: unknown) => {
            if (stopped || recoveryTimer !== undefined || document.hidden) {
                return
            }
            if (recoveryAttemptsRef.current >= kMaximumRecoveryAttempts) {
                console.error('WebGL 恢复失败，重新激活窗口后重试', error)
                return
            }
            const delay =
                kRecoveryDelayMilliseconds * 2 ** recoveryAttemptsRef.current
            recoveryTimer = window.setTimeout(() => {
                recoveryAttemptsRef.current += 1
                // Recreate only this canvas when the driver cannot restore it.
                setContextGeneration((value) => value + 1)
            }, delay)
        }
        const initialize = () => {
            cancelRecovery()
            disposeController()
            setAvailable(false)
            try {
                const controller = createGlassController(canvas)
                controllerRef.current = controller
                void controller.ready
                    .then(() => {
                        if (!stopped && controllerRef.current === controller) {
                            recoveryAttemptsRef.current = 0
                            setAvailable(true)
                        }
                    })
                    .catch((error: unknown) => {
                        if (!stopped && controllerRef.current === controller) {
                            disposeController()
                            scheduleRecovery(error)
                        }
                    })
            } catch (error) {
                scheduleRecovery(error)
            }
        }
        const onLost = (event: Event) => {
            event.preventDefault()
            disposeController()
            setAvailable(false)
            scheduleRecovery()
        }
        const onResume = () => {
            if (document.hidden) {
                return
            }
            if (controllerRef.current) {
                controllerRef.current.update()
            } else if (recoveryTimer === undefined) {
                recoveryAttemptsRef.current = 0
                scheduleRecovery()
            }
        }
        canvas.addEventListener('webglcontextlost', onLost)
        canvas.addEventListener('webglcontextrestored', initialize)
        document.addEventListener('visibilitychange', onResume)
        window.addEventListener('focus', onResume)
        initialize()
        return () => {
            stopped = true
            canvas.removeEventListener('webglcontextlost', onLost)
            canvas.removeEventListener('webglcontextrestored', initialize)
            document.removeEventListener('visibilitychange', onResume)
            window.removeEventListener('focus', onResume)
            cancelRecovery()
            disposeController()
        }
    }, [contextGeneration])

    useEffect(() => {
        controllerRef.current?.update()
    }, [activeTab])

    return (
        <>
            <canvas
                key={contextGeneration}
                ref={canvasRef}
                aria-hidden="true"
                className={`sidebar-glass ${available ? 'ready' : ''}`}
                data-glass-state={available ? 'active' : 'fallback'}
                data-glass-provider="liquid-glass-webgl"
            />
            {fallbackRect && (
                <span
                    aria-hidden="true"
                    className={`sidebar-glass-fallback ${available ? 'hidden' : ''}`}
                    style={{
                        left: fallbackRect.x,
                        top: fallbackRect.y,
                        width: fallbackRect.width,
                        height: fallbackRect.height,
                    }}
                />
            )}
        </>
    )
}
