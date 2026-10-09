import type { LiquidGlassRenderer } from './vendor/liquid-glass/renderer'
import type { ControlEntry } from './control-glass-types'
import { copyControlFrame } from './control-glass-elements'

/** Render changed surfaces separately; unchanged DOM canvases keep their pixels. */
export function paintControlFrames(
    entries: Iterable<ControlEntry>,
    renderer: LiquidGlassRenderer,
    render: () => void,
): void {
    const allElements = renderer.buttonConfigs
    const dirtyIds = new Set(renderer.dirtyElementIds)
    const allDirty = renderer.allDirty
    try {
        for (const entry of entries) {
            if (
                !entry.visible ||
                !entry.elements.length ||
                (!allDirty &&
                    !entry.dirty &&
                    !entry.elements.some((element) => dirtyIds.has(element.id)))
            ) {
                continue
            }
            // Keep the full element list between frames for upstream animation
            // and resource ownership. Only the current surface shares a backdrop.
            renderer.buttonConfigs = entry.elements
            renderer.allDirty = true
            renderer.needsRedraw = true
            render()
            copyControlFrame(entry, renderer)
            entry.dirty = false
        }
    } finally {
        renderer.buttonConfigs = allElements
        renderer.allDirty = false
        renderer.dirtyElementIds.clear()
        renderer.needsRedraw = false
    }
}
