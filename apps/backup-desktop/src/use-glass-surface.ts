import { useEffect, useRef } from 'react'
import type { ControlOptions, ControlRegistration } from './control-glass-types'

/** Attach a visual surface without taking ownership of native DOM input. */
export function useGlassSurface(options: ControlOptions) {
    const surface = useRef<HTMLCanvasElement>(null)
    const latest = useRef(options)
    const registration = useRef<ControlRegistration | null>(null)
    latest.current = options
    useEffect(() => {
        let disposed = false
        void import('./control-glass')
            .then(({ registerGlassControl }) => {
                if (!disposed && surface.current) {
                    registration.current = registerGlassControl(
                        surface.current,
                        () => latest.current,
                    )
                }
            })
            .catch((error: unknown) => {
                console.error('玻璃控件加载失败', error)
            })
        return () => {
            disposed = true
            registration.current?.dispose()
            registration.current = null
        }
    }, [])
    const state =
        options.kind === 'switch'
            ? options.checked
            : options.kind === 'button'
              ? options.variant
              : options.kind
    useEffect(() => registration.current?.update(), [state])
    return surface
}
