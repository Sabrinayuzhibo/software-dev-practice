// Extracted from upstream catalog/build-dialog.ts. See ../NOTICE.md.
import type { GlassElementConfig } from '../renderer'
import { DEFAULT_HIGHLIGHT, DP } from './constants'
import { makeGlassShape } from './helpers-elements'
import { LIGHT_PALETTE } from './palettes'

/** The upstream dialog material, with layout and text owned by native DOM. */
export function makeDialogGlass(
    id: string,
    rect: { x: number; y: number; w: number; h: number },
): GlassElementConfig {
    const card = makeGlassShape(
        id,
        rect,
        {
            cornerRadius: 48 * DP,
            refractionHeight: 24 * DP,
            refractionAmount: -48 * DP,
            blurRadius: LIGHT_PALETTE.dialogBlurRadius,
            saturation: 1.5,
            brightness: LIGHT_PALETTE.dialogBrightness,
            surfaceColor: LIGHT_PALETTE.dialogContainer,
            highlight: {
                ...DEFAULT_HIGHLIGHT,
                mode: 2,
                color: [1, 1, 1],
                alpha: 0.38,
                widthDp: 0.5,
            },
            depthEffect: true,
        },
        false,
    )
    card.useSeparableBlur = true
    card.independentBackdrop = false
    return card
}
