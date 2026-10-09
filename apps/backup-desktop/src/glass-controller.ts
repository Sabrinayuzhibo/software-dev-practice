import { makeButton } from './vendor/liquid-glass/catalog/helpers-elements'
import { LiquidGlassRenderer } from './vendor/liquid-glass/renderer'
import wallpaperUrl from './vendor/liquid-glass/wallpaper_light.webp'
import { configureGlassRenderer } from './glass-settings'

export interface GlassController {
    ready: Promise<void>
    update: () => void
    dispose: () => void
}

function navigationElements(sidebar: HTMLElement) {
    const outer = sidebar.getBoundingClientRect()
    return Array.from(
        sidebar.querySelectorAll<HTMLButtonElement>('.nav-item'),
        (button) => {
            const rect = button.getBoundingClientRect()
            const selected = button.classList.contains('active')
            const element = makeButton(
                button.dataset.tab!,
                {
                    x: rect.left - outer.left,
                    y: rect.top - outer.top,
                    w: rect.width,
                    h: rect.height,
                },
                {
                    label: '',
                    // Same Surface / Tinted Blue presets as build-buttons.ts.
                    tintColor: selected ? [0, 0x88 / 255, 1, 1] : [0, 0, 0, 0],
                    surfaceColor: selected ? [0, 0, 0, 0] : [1, 1, 1, 0.3],
                    labelColor: [0, 0, 0, 0],
                },
                false,
            )
            element.useContinuousSdf = true
            element.useSeparableBlur = true
            return element
        },
    )
}

/** Attach upstream glass rendering to DOM navigation; never owns task state. */
export function createGlassController(
    canvas: HTMLCanvasElement,
): GlassController {
    const sidebar = canvas.parentElement
    if (!sidebar) {
        throw new Error('侧栏尚未挂载')
    }
    const renderer = new LiquidGlassRenderer(canvas)
    configureGlassRenderer(renderer)

    let disposed = false
    let pressed: string | undefined
    const motion = window.matchMedia('(prefers-reduced-motion: reduce)')
    const update = () => {
        if (disposed || renderer.gl.isContextLost()) {
            return
        }
        const ratio = Math.min(window.devicePixelRatio || 1, 3)
        if (
            renderer.cssWidth !== sidebar.clientWidth ||
            renderer.cssHeight !== sidebar.clientHeight ||
            renderer.dpr !== ratio
        ) {
            renderer.dpr = ratio
            renderer.resize(sidebar.clientWidth, sidebar.clientHeight)
        }
        renderer.setElements(navigationElements(sidebar))
    }
    const release = () => {
        if (pressed && !disposed) {
            renderer.setPressed(pressed, false)
            pressed = undefined
        }
    }
    const point = (event: PointerEvent) => {
        const rect = sidebar.getBoundingClientRect()
        return { x: event.clientX - rect.left, y: event.clientY - rect.top }
    }
    const onDown = (event: PointerEvent) => {
        const button = (event.target as Element).closest<HTMLButtonElement>(
            '.nav-item',
        )
        if (!button || event.button !== 0 || motion.matches || disposed) {
            return
        }
        release()
        pressed = button.dataset.tab
        if (pressed) {
            renderer.setPressed(pressed, true, point(event))
        }
    }
    const onMove = (event: PointerEvent) => {
        if (pressed && !disposed) {
            renderer.setDragPosition(pressed, point(event))
        }
    }
    const observer = new ResizeObserver(update)
    let resolution = window.matchMedia(
        `(resolution: ${window.devicePixelRatio}dppx)`,
    )
    const onResolution = () => {
        resolution.removeEventListener('change', onResolution)
        resolution = window.matchMedia(
            `(resolution: ${window.devicePixelRatio}dppx)`,
        )
        resolution.addEventListener('change', onResolution)
        update()
    }
    const ready = renderer.loadWallpaper(wallpaperUrl).then(() => {
        // Upstream image loading is asynchronous and cannot be cancelled.
        // Dispose again if it completed after unmount/context replacement.
        if (disposed) {
            renderer.dispose()
            return
        }
        update()
        renderer.render()
        if (
            renderer.gl.isContextLost() ||
            renderer.gl.getError() !== renderer.gl.NO_ERROR
        ) {
            throw new Error('原版玻璃渲染器首帧绘制失败')
        }
    })
    observer.observe(sidebar)
    resolution.addEventListener('change', onResolution)
    sidebar.addEventListener('pointerdown', onDown)
    window.addEventListener('pointermove', onMove)
    window.addEventListener('pointerup', release)
    window.addEventListener('pointercancel', release)
    window.addEventListener('blur', release)
    update()
    return {
        ready,
        update,
        dispose: () => {
            disposed = true
            observer.disconnect()
            resolution.removeEventListener('change', onResolution)
            sidebar.removeEventListener('pointerdown', onDown)
            window.removeEventListener('pointermove', onMove)
            window.removeEventListener('pointerup', release)
            window.removeEventListener('pointercancel', release)
            window.removeEventListener('blur', release)
            renderer.dispose()
        },
    }
}
