import { LiquidGlassRenderer } from './vendor/liquid-glass/renderer'
import { configureGlassRenderer } from './glass-settings'
import wallpaperUrl from './vendor/liquid-glass/wallpaper_light.webp'
import { layoutControls } from './control-glass-elements'
import { paintControlFrames } from './control-glass-paint'
import { bindControlInput } from './control-glass-input'
import type {
    ControlEntry,
    ControlOptions,
    ControlPool,
    ControlRegistration,
} from './control-glass-types'

const kMaximumRecoveryAttempts = 4
let nextId = 0
const sharedRenderers = new Map<ControlPool, ControlGlass>()

/** Separate large static surfaces from the small animated control buffer. */
class ControlGlass {
    private entries = new Set<ControlEntry>()
    private renderer: LiquidGlassRenderer | null = null
    private canvas: HTMLCanvasElement | null = null
    private frame = 0
    private recoveryTimer = 0
    private recoveryAttempts = 0
    private stopped = false
    private resizeObserver = new ResizeObserver(() => this.update())
    private intersectionObserver = new IntersectionObserver((changes) => {
        for (const change of changes) {
            for (const entry of this.entries) {
                if (entry.host === change.target) {
                    entry.visible = change.isIntersecting
                }
            }
        }
        this.update()
    })

    constructor(private pool: ControlPool) {
        window.addEventListener('focus', this.resume)
        window.addEventListener('resize', this.update)
        document.addEventListener('visibilitychange', this.resume)
        this.initialize()
    }

    private initialize = () => {
        if (this.stopped) {
            return
        }
        window.clearTimeout(this.recoveryTimer)
        this.recoveryTimer = 0
        this.destroyRenderer()
        const canvas = document.createElement('canvas')
        canvas.className = `${this.pool}-glass-source`
        canvas.hidden = true
        canvas.setAttribute('aria-hidden', 'true')
        document.body.append(canvas)
        this.canvas = canvas
        canvas.addEventListener('webglcontextlost', this.lost)
        canvas.addEventListener('webglcontextrestored', this.initialize)
        try {
            const renderer = new LiquidGlassRenderer(canvas)
            this.renderer = renderer
            configureGlassRenderer(renderer)
            const render = renderer.render.bind(renderer)
            renderer.render = () => {
                if (
                    renderer.gl.isContextLost() ||
                    !renderer.needsRedraw ||
                    !renderer.cssWidth ||
                    !renderer.cssHeight
                ) {
                    return
                }
                try {
                    paintControlFrames(this.entries, renderer, render)
                    // getError synchronizes SwiftShader's queue and stalls large
                    // sheets; context loss and thrown failures use fail().
                    canvas.dataset.glassState = 'active'
                    this.recoveryAttempts = 0
                } catch (error) {
                    this.fail(error)
                }
            }
            // The toggle shader also binds the wallpaper sampler on solid
            // backdrops. Keep its texture complete, as the upstream canvas does.
            void renderer
                .loadWallpaper(wallpaperUrl)
                .then(() => {
                    if (this.renderer !== renderer) {
                        renderer.dispose()
                        return
                    }
                    if (this.pool !== 'sheet') {
                        renderer.setBackgroundColor([1, 1, 1])
                    }
                    this.update()
                })
                .catch((error: unknown) => {
                    if (this.renderer === renderer) {
                        this.fail(error)
                    }
                })
        } catch (error) {
            this.fail(error)
        }
    }

    private fail(error?: unknown) {
        if (this.canvas) {
            this.canvas.dataset.glassState = 'fallback'
            this.canvas.dataset.glassError = String(error ?? 'Context lost')
        }
        this.renderer?.dispose()
        this.renderer = null
        for (const entry of this.entries) {
            entry.host.dataset.glassState = 'fallback'
            if (entry.options().kind === 'button') {
                const label = entry.host.querySelector<HTMLElement>(
                    '.glass-button-label',
                )
                if (label) {
                    label.style.transform = ''
                }
            }
        }
        if (this.stopped || this.recoveryTimer || document.hidden) {
            return
        }
        if (this.recoveryAttempts >= kMaximumRecoveryAttempts) {
            console.error('玻璃控件恢复失败，重新激活窗口后重试', error)
            return
        }
        this.recoveryTimer = window.setTimeout(
            this.initialize,
            1000 * 2 ** this.recoveryAttempts++,
        )
    }

    private lost = (event: Event) => {
        event.preventDefault()
        this.fail()
    }

    private resume = () => {
        if (document.hidden) {
            return
        }
        if (!this.renderer && !this.recoveryTimer) {
            this.recoveryAttempts = 0
            this.initialize()
        }
        this.update()
    }

    update = () => {
        if (this.frame || this.stopped) {
            return
        }
        this.frame = requestAnimationFrame(() => {
            this.frame = 0
            if (this.renderer?.wallpaperReady) {
                try {
                    layoutControls([...this.entries], this.renderer)
                } catch (error) {
                    this.fail(error)
                }
            }
        })
    }

    register(entry: ControlEntry): ControlRegistration {
        this.entries.add(entry)
        this.resizeObserver.observe(entry.host)
        this.intersectionObserver.observe(entry.host)
        const unbind =
            this.pool === 'control'
                ? bindControlInput(entry, () => this.renderer)
                : () => {}
        this.update()
        return {
            update: this.update,
            dispose: () => {
                unbind()
                this.entries.delete(entry)
                this.renderer?.toggleStates.delete(entry.id)
                this.resizeObserver.unobserve(entry.host)
                this.intersectionObserver.unobserve(entry.host)
                if (this.entries.size === 0) {
                    this.dispose()
                    sharedRenderers.delete(this.pool)
                } else {
                    this.update()
                }
            },
        }
    }

    private destroyRenderer() {
        this.renderer?.dispose()
        this.renderer = null
        this.canvas?.removeEventListener('webglcontextlost', this.lost)
        this.canvas?.removeEventListener(
            'webglcontextrestored',
            this.initialize,
        )
        this.canvas?.remove()
        this.canvas = null
    }

    private dispose() {
        this.stopped = true
        cancelAnimationFrame(this.frame)
        window.clearTimeout(this.recoveryTimer)
        this.resizeObserver.disconnect()
        this.intersectionObserver.disconnect()
        window.removeEventListener('focus', this.resume)
        window.removeEventListener('resize', this.update)
        document.removeEventListener('visibilitychange', this.resume)
        this.destroyRenderer()
    }
}

/** Register a native control or dialog; dispose its surface on unmount. */
export function registerGlassControl(
    canvas: HTMLCanvasElement,
    options: () => ControlOptions,
): ControlRegistration {
    const host = canvas.parentElement!
    const input = host.querySelector<HTMLElement>('input, select') ?? host
    const context = canvas.getContext('2d')
    if (!context) {
        throw new Error('无法创建玻璃控件显示层')
    }
    const kind = options().kind
    const pool = kind === 'field' || kind === 'sheet' ? kind : 'control'
    let renderer = sharedRenderers.get(pool)
    if (!renderer) {
        renderer = new ControlGlass(pool)
        sharedRenderers.set(pool, renderer)
    }
    return renderer.register({
        id: `control-${++nextId}`,
        host,
        input,
        canvas,
        context,
        options,
        rect: { x: 0, y: 0, w: 0, h: 0 },
        elements: [],
        dirty: true,
        visible: true,
    })
}
