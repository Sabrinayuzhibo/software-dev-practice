import type { LiquidGlassRenderer } from './vendor/liquid-glass/renderer'
import type { ControlEntry } from './control-glass-types'

const kDragThreshold = 3
const kToggleTravel = 20

/** Pointer position is immediate; upstream springs still animate deformation. */
function dragControlToggle(
    renderer: LiquidGlassRenderer,
    id: string,
    fraction: number,
    currentX: number,
    startX: number,
): void {
    renderer.dragToggle(id, fraction, currentX, startX, kToggleTravel)
    const state = renderer.toggleStates.get(id)
    if (!state?.isDragging) {
        return
    }
    state.fraction = state.targetFraction
    state.fractionVelocity = 0
    state.velocityTracker.addPosition(performance.now(), state.fraction)
    state.targetVelocity = state.velocityTracker.calculateVelocity()
    renderer.requestRender()
}

/** Visual gestures never replace native button clicks or form submission. */
export function bindControlInput(
    entry: ControlEntry,
    getRenderer: () => LiquidGlassRenderer | null,
): () => void {
    const input: HTMLElement = entry.input
    let gesture: {
        pointerId: number
        x: number
        fraction: number
        dragged: boolean
    } | null = null
    let suppressClick = false
    let keyboardPressed = false
    const reducedMotion = () =>
        matchMedia('(prefers-reduced-motion: reduce)').matches
    const enabled = () => !entry.input.matches(':disabled')
    const point = (event: PointerEvent) => {
        const bounds = entry.host.getBoundingClientRect()
        return {
            x: event.clientX - bounds.x + entry.rect.x,
            y: event.clientY - bounds.y + entry.rect.y,
        }
    }
    const down = (event: PointerEvent) => {
        const renderer = getRenderer()
        if (!renderer || !enabled() || event.button !== 0 || gesture) {
            return
        }
        suppressClick = false
        const options = entry.options()
        const fraction =
            options.kind === 'switch'
                ? (renderer.toggleStates.get(entry.id)?.fraction ??
                  Number(options.checked))
                : 0
        gesture = {
            pointerId: event.pointerId,
            x: event.clientX,
            fraction,
            dragged: false,
        }
        if (options.kind === 'switch') {
            entry.input.setPointerCapture(event.pointerId)
            if (!reducedMotion()) {
                renderer.beginToggleDrag(entry.id, fraction)
            }
        } else if (!reducedMotion()) {
            renderer.setPressed(entry.id, true, point(event))
        }
    }
    const move = (event: PointerEvent) => {
        const renderer = getRenderer()
        if (
            !gesture ||
            event.pointerId !== gesture.pointerId ||
            !renderer ||
            !enabled()
        ) {
            return
        }
        if (entry.options().kind === 'switch') {
            gesture.dragged ||=
                Math.abs(event.clientX - gesture.x) > kDragThreshold
            if (!reducedMotion()) {
                dragControlToggle(
                    renderer,
                    entry.id,
                    gesture.fraction,
                    event.clientX,
                    gesture.x,
                )
            }
        } else if (!reducedMotion()) {
            renderer.setDragPosition(entry.id, point(event))
        }
    }
    const release = (event?: PointerEvent) => {
        if (event && gesture && event.pointerId !== gesture.pointerId) {
            return
        }
        const previous = gesture
        gesture = null
        const renderer = getRenderer()
        if (!renderer) {
            return
        }
        const options = entry.options()
        if (options.kind === 'button') {
            if (previous || keyboardPressed) {
                renderer.setPressed(entry.id, false)
                keyboardPressed = false
            }
            return
        }
        if (options.kind !== 'switch' || !previous) {
            return
        }
        if (entry.input.hasPointerCapture(previous.pointerId)) {
            entry.input.releasePointerCapture(previous.pointerId)
        }
        const cancelled = !event || event.type !== 'pointerup' || !enabled()
        renderer.endToggleDrag(entry.id)
        if (!cancelled && previous.dragged) {
            const fraction =
                previous.fraction + (event.clientX - previous.x) / kToggleTravel
            suppressClick = true
            options.setChecked(fraction >= 0.5)
        } else {
            renderer.setToggleTarget(entry.id, Number(options.checked))
        }
    }
    const click = (event: MouseEvent) => {
        if (suppressClick && event.detail !== 0) {
            suppressClick = false
            // A drag already committed its value. Suppress only its follow-up
            // pointer click; label and keyboard activation stay native.
            event.preventDefault()
            event.stopImmediatePropagation()
        }
    }
    const keyDown = (event: KeyboardEvent) => {
        if (
            enabled() &&
            !event.repeat &&
            !reducedMotion() &&
            entry.options().kind === 'button' &&
            [' ', 'Enter'].includes(event.key)
        ) {
            keyboardPressed = true
            getRenderer()?.setPressed(entry.id, true)
        }
    }
    const keyUp = () => {
        if (keyboardPressed) {
            keyboardPressed = false
            getRenderer()?.setPressed(entry.id, false)
        }
    }
    const blur = () => release()
    input.addEventListener('pointerdown', down)
    input.addEventListener('click', click, true)
    input.addEventListener('keydown', keyDown)
    input.addEventListener('keyup', keyUp)
    input.addEventListener('blur', blur)
    window.addEventListener('pointermove', move)
    window.addEventListener('pointerup', release)
    window.addEventListener('pointercancel', release)
    window.addEventListener('blur', blur)
    return () => {
        release()
        input.removeEventListener('pointerdown', down)
        input.removeEventListener('click', click, true)
        input.removeEventListener('keydown', keyDown)
        input.removeEventListener('keyup', keyUp)
        input.removeEventListener('blur', blur)
        window.removeEventListener('pointermove', move)
        window.removeEventListener('pointerup', release)
        window.removeEventListener('pointercancel', release)
        window.removeEventListener('blur', blur)
    }
}
