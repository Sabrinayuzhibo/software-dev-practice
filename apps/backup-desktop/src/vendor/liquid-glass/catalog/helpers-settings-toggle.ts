// Modified 2026-10-09: keep upstream track/knob construction; DOM owns labels
// and input. Import constants/palette directly without the demo catalog.
// Upstream source, license and integration details: ../NOTICE.md.
import type { GlassElementConfig, GlassHighlight } from '../renderer'
import { DP } from './constants'
import type { ThemePalette } from './palettes'
import { makePlainRect, makeGlassShape } from './helpers-elements'

/* ------------------------------------------------------------------ *
 * Settings toggle — a compact toggle switch for the settings page.
 * Creates the original toggle track and knob, arranged in a row.
 * Uses the same Liquid Toggle anatomy as build-toggle.ts but simplified
 * for settings. The application supplies native labels and input handling.
 *
 * Layout: toggle on the right, within the given row.
 * Returns elements to be merged into the caller's arrays.
 * ------------------------------------------------------------------ */
export function makeSettingsToggle(
  id: string,
  rowRect: { x: number; y: number; w: number; h: number },
  palette: ThemePalette,
  scroll = true,
  /** Padding inside the row (for label text alignment). Defaults to 0. */
  labelPad = 0,
  /** When true, the toggle sits on a GLASS card (semi-transparent, over
   *  wallpaper) instead of a solid-color card. In that case we must NOT
   *  set solidBackdropColor — the knob should sample the real backdrop
   *  (the rendered glass sheet) so it picks up the sheet's tint/blur.
   *  Setting solidBackdropColor on a glass card makes the knob render
   *  with a wrong (often dark, in dark-theme) flat color → "knob全黑". */
  onGlassCard = false,
): GlassElementConfig[] {
  const elements: GlassElementConfig[] = []

  // Toggle dimensions (same as LiquidToggle.kt)
  const TOGGLE_W = 64 * DP
  const TOGGLE_H = 28 * DP
  const TOGGLE_KNOB_W = 40 * DP
  const TOGGLE_KNOB_H = 24 * DP
  const TOGGLE_DRAG = 20 * DP
  const TOGGLE_PADDING = 2 * DP

  // Layout: label fills the full row width/height for press tint and hit area.
  // Toggle track is positioned inside the row (respecting labelPad on the right).
  const trackX = rowRect.x + rowRect.w - TOGGLE_W - labelPad
  const trackY = rowRect.y + (rowRect.h - TOGGLE_H) / 2
  const knobX = trackX + TOGGLE_PADDING
  const knobY = trackY + (TOGGLE_H - TOGGLE_KNOB_H) / 2

  // Track
  const trackColorOff = palette.toggleTrackOff
  const accentColor = palette.toggleAccent
  const trackEl = makePlainRect(
    `${id}-track`,
    { x: trackX, y: trackY, w: TOGGLE_W, h: TOGGLE_H },
    trackColorOff,
    TOGGLE_H / 2,
    scroll,
  )
  trackEl.isToggleTrack = {
    groupId: id,
    offColor: trackColorOff,
    onColor: [...accentColor, 1] as [number, number, number, number],
  }
  elements.push(trackEl)

  // Knob — same glass effects as the full LiquidToggle knob.
  // No CombinedBackdrop props (solidBackdropColor, trackColorOff/On, etc.)
  // — matches slider knob behavior so the backdrop scrolls correctly.
  const KNOB_HIGHLIGHT: GlassHighlight = {
    mode: 1,
    color: [1, 1, 1],
    angle: Math.PI / 4,
    falloff: 1.0,
    alpha: 1.0,
    widthDp: 0.5 / 1.5,
    blurRadiusDp: 0.25 / 1.5,
  }
  const knobEl = makeGlassShape(
    `${id}-knob`,
    { x: knobX, y: knobY, w: TOGGLE_KNOB_W, h: TOGGLE_KNOB_H },
    {
      cornerRadius: TOGGLE_KNOB_H / 2,
      refractionHeight: 5 * DP,
      refractionAmount: -10 * DP,
      blurRadius: 8 * DP,
      saturation: 1.0,
      surfaceColor: [0, 0, 0, 0],
      highlight: KNOB_HIGHLIGHT,
      outerShadow: { radius: 4 * DP, alpha: 0.05, offsetX: 0, offsetY: (4 / 6) * DP, color: [0, 0, 0] },
      innerShadow: { radius: 4 * DP, alpha: 0.3, offsetX: 0, offsetY: 4 * DP },
      chromaticAberration: true,
    },
    scroll,
  )
  // CombinedBackdrop — faithful to LiquidToggle.kt:
  //   backdrop = rememberCombinedBackdrop(backdrop, scaled trackBackdrop)
  // Settings toggles are on a solid-color card, so:
  //   - outer backdrop = CanvasBackdrop (card color) → solidBackdropColor
  //   - track color lerps between offColor and onColor by fraction
  knobEl.isToggleKnob = {
    groupId: id,
    dragWidth: TOGGLE_DRAG,
    trackColorOff: palette.toggleTrackOff,
    trackColorOn: [...palette.toggleAccent, 1] as [number, number, number, number],
    trackW: TOGGLE_W,
    trackH: TOGGLE_H,
    trackOriginalX: trackX,
    trackOriginalY: trackY,
    // Only set solidBackdropColor when the toggle is on a SOLID card (settings
    // page). On a glass card (TextGlass sheet), omit it so the knob samples
    // the real backdrop (the rendered glass sheet) instead of a flat color.
    ...(onGlassCard ? {} : { solidBackdropColor: palette.toggleCardBg }),
  }
  elements.push(knobEl)

  return elements
}
