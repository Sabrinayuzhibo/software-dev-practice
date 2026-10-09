import type { InputHTMLAttributes, SelectHTMLAttributes } from 'react'
import { ChevronDown } from 'lucide-react'
import { useGlassSurface } from './use-glass-surface'
import './glass-surfaces.css'

/** Native text, number and readonly inputs over the upstream input surface. */
export function GlassInput(props: InputHTMLAttributes<HTMLInputElement>) {
    const surface = useGlassSurface({ kind: 'field' })
    return (
        <span className="glass-field">
            <canvas
                ref={surface}
                className="glass-surface"
                aria-hidden="true"
            />
            <input {...props} />
        </span>
    )
}

/** Keep the platform menu, keyboard navigation and form validation intact. */
export function GlassSelect({
    children,
    ...props
}: SelectHTMLAttributes<HTMLSelectElement>) {
    const surface = useGlassSurface({ kind: 'field' })
    return (
        <span className="glass-field glass-select">
            <canvas
                ref={surface}
                className="glass-surface"
                aria-hidden="true"
            />
            <select {...props}>{children}</select>
            <ChevronDown
                size={15}
                className="glass-select-chevron"
                aria-hidden="true"
            />
        </span>
    )
}
