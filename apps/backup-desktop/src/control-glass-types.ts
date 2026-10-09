import type { GlassElementConfig } from './vendor/liquid-glass/renderer'

export type ControlOptions =
    | { kind: 'button'; variant: 'surface' | 'primary' | 'danger' }
    | { kind: 'switch'; checked: boolean; setChecked: (value: boolean) => void }
    | { kind: 'field' }
    | { kind: 'sheet' }

export type ControlPool = 'control' | 'field' | 'sheet'

export interface ControlRegistration {
    update: () => void
    dispose: () => void
}

export const kControlPadding = 12

export interface ControlEntry {
    id: string
    host: HTMLElement
    input: HTMLElement
    canvas: HTMLCanvasElement
    context: CanvasRenderingContext2D
    options: () => ControlOptions
    rect: { x: number; y: number; w: number; h: number }
    elements: GlassElementConfig[]
    dirty: boolean
    visible: boolean
}
