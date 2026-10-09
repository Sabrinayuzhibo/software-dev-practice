# Liquid Glass WebGL

Vendored on 2026-10-09 from
https://github.com/martin65536/liquid-glass-webgl
at commit `0b90f9fc1cea8c7e59825f9aff1cbb96674b5b80`.

License: GNU Affero General Public License version 3 (AGPL-3.0),
included in `LICENSE` and the desktop distribution's
`licenses/liquid-glass-webgl.txt`. This notice is also shipped as
`licenses/liquid-glass-webgl-NOTICE.md`.

Credits from the upstream project:

- Web port: martin65536 and Z.ai Agent.
- Original design and Android implementation: Kyant,
  https://github.com/Kyant0/AndroidLiquidGlass.

Imported files:

- `renderer/` and `shaders/`: upstream WebGL pipeline, shaders,
  geometry, caching, and spring animations, retained without reformatting.
- `catalog/constants.ts` and `catalog/helpers-elements.ts`: upstream
  glass presets and element factories.
- `catalog/helpers-settings-toggle.ts` and `catalog/palettes.ts`: original
  Liquid Toggle track/knob construction and theme colors.
- `catalog/helpers-text-input.ts`: original capsule input material.
- `catalog/helpers-dialog.ts`: dialog material extracted from the upstream
  `catalog/build-dialog.ts`, retaining its optical parameters and light palette.
- `wallpaper_light.webp`: upstream `public/wallpaper/wallpaper_light.webp`.

Local modifications:

- `catalog/helpers-elements.ts` imports `./constants` instead of the
  catalog's `./types` barrel, avoiding unrelated demo dependencies.
  The text input helper uses the same import adaptation.
- Strict TypeScript integration fixes: guard optional plain-rect and blur
  specs, align glass/toggle method declarations with their implementations,
  fill missing diagnostic blur-stat fields, and remove a reset of an
  undeclared, unused performance counter. Shader sources are unchanged.
- `renderer/gl-utils.ts` checks shader/program allocation and releases
  compiled shaders after linking, including failure paths.
- `catalog/helpers-settings-toggle.ts` retains the track/knob construction
  and removes the demo label and interaction map; accessible DOM controls
  handle input. Imports use constants and palettes directly.
- Plain-rectangle and shadow passes bind their inactive SDF sampler to the
  existing placeholder texture. This prevents a WebGL framebuffer feedback
  loop when unit 0 still holds the scene texture. Its uniform is cached in
  `methods-uniforms.ts`, and `methods-dispose.ts` releases the placeholder.

The sidebar adapter is `../../glass-controller.ts`. Other surfaces use
`../../control-glass.ts`, `../../glass-controls.tsx` and `../../glass-fields.tsx`.
Animated buttons/toggles, static fields and dialogs have separate shared WebGL
renderers, so a large dialog never enlarges the animation scratch buffer.
Each renderer draws only changed surfaces through a reusable scratch buffer,
then copies them to DOM canvases. Static surfaces retain their pixels.
A feathered mask removes the rectangular scratch background. Toggle position
follows pointer input directly; upstream springs still animate press deformation
and release. The renderer and shader effects remain the same.
Buttons use upstream Surface / Tinted Blue parameters, with a red tint for
destructive commands. Buttons, toggles and fields use their local solid
backdrop. The sidebar and dialogs use the original wallpaper; dialogs include
the upstream light scrim. These canvases do not capture or refract live DOM
content behind them. DOM text, labels, disabled state, form behavior, modal
focus and the platform select menu stay native. Data tables, task cards,
status labels, disclosures, progress and scrollbars use locally adapted CSS
in `../../glass-theme.css`, not additional upstream demo builders. The
application's layout differs from the demo.

When distributing the combined application, comply with AGPL-3.0, including
the applicable corresponding-source and license-notice requirements. The
upstream repository alone is not the corresponding source of this modified
combined application; provide the matching application source and build
instructions as well. A network deployment must also satisfy section 13.
