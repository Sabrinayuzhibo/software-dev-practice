import {
    makeButton,
    makePlainRect,
} from './vendor/liquid-glass/catalog/helpers-elements'
import { makeSettingsToggle } from './vendor/liquid-glass/catalog/helpers-settings-toggle'
import { makeTextInputGlass } from './vendor/liquid-glass/catalog/helpers-text-input'
import { makeDialogGlass } from './vendor/liquid-glass/catalog/helpers-dialog'
import { LIGHT_PALETTE } from './vendor/liquid-glass/catalog/palettes'
import type {
    GlassElementConfig,
    LiquidGlassRenderer,
} from './vendor/liquid-glass/renderer'
import { kControlPadding, type ControlEntry } from './control-glass-types'
import { computeElementTransform } from './vendor/liquid-glass/renderer/methods-render-glass-transform'

const backgroundCanvas = document.createElement('canvas')
backgroundCanvas.width = backgroundCanvas.height = 1
const backgroundContext = backgroundCanvas.getContext('2d', {
    willReadFrequently: true,
})!

function backgroundColor(host: HTMLElement): [number, number, number, number] {
    const colors: string[] = []
    for (
        let parent = host.parentElement;
        parent;
        parent = parent.parentElement
    ) {
        colors.push(getComputedStyle(parent).backgroundColor)
    }
    backgroundContext.fillStyle = '#f7f8fa'
    backgroundContext.fillRect(0, 0, 1, 1)
    for (const color of colors.reverse()) {
        backgroundContext.fillStyle = color
        backgroundContext.fillRect(0, 0, 1, 1)
    }
    const [red, green, blue] = backgroundContext.getImageData(0, 0, 1, 1).data
    return [red / 255, green / 255, blue / 255, 1]
}

function controlElements(entry: ControlEntry): GlassElementConfig[] {
    const options = entry.options()
    const color = backgroundColor(entry.host)
    const rect = entry.rect
    const backdrop = makePlainRect(
        `${entry.id}-backdrop`,
        {
            x: rect.x - kControlPadding,
            y: rect.y - kControlPadding,
            w: rect.w + kControlPadding * 2,
            h: rect.h + kControlPadding * 2,
        },
        options.kind === 'sheet' ? LIGHT_PALETTE.dialogDim : color,
        0,
        false,
    )
    if (options.kind === 'field') {
        return [backdrop, makeTextInputGlass(entry.id, rect)]
    }
    if (options.kind === 'sheet') {
        return [backdrop, makeDialogGlass(entry.id, rect)]
    }
    if (options.kind === 'switch') {
        return [
            backdrop,
            ...makeSettingsToggle(
                entry.id,
                rect,
                {
                    ...LIGHT_PALETTE,
                    toggleCardBg: color,
                },
                false,
            ),
        ]
    }
    const element = makeButton(
        entry.id,
        rect,
        {
            label: '',
            tintColor:
                options.variant === 'primary'
                    ? [0, 0x88 / 255, 1, 1]
                    : options.variant === 'danger'
                      ? [0xb8 / 255, 0x32 / 255, 0x32 / 255, 1]
                      : [0, 0, 0, 0],
            surfaceColor:
                options.variant === 'surface' ? [1, 1, 1, 0.3] : [0, 0, 0, 0],
            labelColor: [0, 0, 0, 0],
        },
        false,
    )
    element.useContinuousSdf = true
    element.useSeparableBlur = true
    return [backdrop, element]
}

/** All surfaces reuse one control-sized buffer; DOM owns their final layout. */
export function layoutControls(
    entries: ControlEntry[],
    renderer: LiquidGlassRenderer,
): void {
    const visible = entries.filter((entry) => {
        if (entry.visible && entry.host.checkVisibility()) {
            return true
        }
        entry.rect = { x: 0, y: 0, w: 0, h: 0 }
        entry.elements = []
        return false
    })
    let width = 1
    let height = 1
    const elements: GlassElementConfig[] = []
    const ratio = Math.min(window.devicePixelRatio || 1, 3)
    for (const entry of visible) {
        const bounds = entry.host.getBoundingClientRect()
        const options = entry.options()
        const w =
            options.kind === 'sheet'
                ? entry.host.clientWidth
                : Math.ceil(bounds.width)
        const h =
            options.kind === 'sheet'
                ? entry.host.clientHeight
                : Math.ceil(bounds.height)
        entry.rect = {
            x: kControlPadding,
            y: kControlPadding,
            w,
            h,
        }
        width = Math.max(width, w + kControlPadding * 2)
        height = Math.max(height, h + kControlPadding * 2)
        if (options.kind === 'switch') {
            if (matchMedia('(prefers-reduced-motion: reduce)').matches) {
                renderer.toggleStates.delete(entry.id)
            }
            renderer.ensureToggleState(entry.id, Number(options.checked))
            renderer.setToggleTarget(entry.id, Number(options.checked))
        }
        entry.elements = controlElements(entry)
        entry.dirty = true
        elements.push(...entry.elements)
    }
    if (
        renderer.cssWidth !== width ||
        renderer.cssHeight !== height ||
        renderer.dpr !== ratio
    ) {
        renderer.dpr = ratio
        renderer.resize(width, height)
    }
    renderer.setElements(elements)
}

/** Copy immediately after render, before Chromium discards the framebuffer. */
export function copyControlFrame(
    entry: ControlEntry,
    renderer: LiquidGlassRenderer,
): void {
    const { x, y, w, h } = entry.rect
    if (!entry.visible || !w || !h) {
        return
    }
    // Dialogs have a DOM shadow and never deform. Crop their scratch margin
    // so the display canvas does not enlarge the modal's scrollable bounds.
    const padding = entry.options().kind === 'sheet' ? 0 : kControlPadding
    const width = Math.round((w + padding * 2) * renderer.dpr)
    const height = Math.round((h + padding * 2) * renderer.dpr)
    if (entry.canvas.width !== width || entry.canvas.height !== height) {
        entry.canvas.width = width
        entry.canvas.height = height
    }
    entry.context.clearRect(0, 0, width, height)
    entry.context.drawImage(
        renderer.canvas,
        Math.round((x - padding) * renderer.dpr),
        Math.round((y - padding) * renderer.dpr),
        width,
        height,
        0,
        0,
        width,
        height,
    )
    // Remove the scratch rectangle around each surface. Keep the upstream
    // animated geometry and feather its shadow into the actual DOM backdrop.
    const context = entry.context
    context.save()
    context.scale(renderer.dpr, renderer.dpr)
    context.globalCompositeOperation = 'destination-in'
    context.beginPath()
    for (const element of entry.elements) {
        if (
            element.id !== entry.id &&
            element.id !== `${entry.id}-knob` &&
            element.id !== `${entry.id}-track`
        ) {
            continue
        }
        const transform = computeElementTransform.call(
            renderer,
            element,
            renderer.buttonStates.get(element.id),
            element.rect,
        )
        context.roundRect(
            transform.sx - x + padding - 1,
            transform.sy - y + padding - 1,
            transform.sw + 2,
            transform.sh + 2,
            Math.min(
                element.cornerRadius ?? transform.sh / 2,
                transform.sh / 2,
            ),
        )
        if (element.kind === 'button') {
            const label = entry.host.querySelector<HTMLElement>(
                '.glass-button-label',
            )
            if (label) {
                label.style.transform = `translate(${transform.translationX}px, ${transform.translationY}px) scale(${transform.scaleX}, ${transform.scaleY})`
            }
        }
    }
    context.fillStyle = '#000'
    context.shadowColor = '#000'
    context.shadowBlur = 6 * renderer.dpr
    context.fill()
    context.restore()
    entry.host.dataset.glassState = 'active'
}
