import type { LiquidGlassRenderer } from './vendor/liquid-glass/renderer'

/** Apply the upstream catalog's DEFAULT_STATE without changing its shaders. */
export function configureGlassRenderer(renderer: LiquidGlassRenderer): void {
    renderer.blurTapCap = 9
    renderer.blurDownsample = 1
    renderer.dynamicBlurDownsample = false
    renderer.noContinuousSdf = true
    renderer.capsuleSdfQuality = 0.5
    renderer.usePerElementFbo = true
    renderer.quickToggles.perElementFbo = true
    renderer.useKawaseBlur = true
    renderer.useBlurCache = true
    renderer.kawaseQuality = 1
    renderer.perfMonitor.enabled = false
}
