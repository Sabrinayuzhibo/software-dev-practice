import { type ButtonHTMLAttributes, type InputHTMLAttributes } from 'react'
import { useGlassSurface } from './use-glass-surface'
import './glass-controls.css'

/** Native button semantics, with the upstream renderer supplying its surface. */
export function GlassButton({
    children,
    className = '',
    ...props
}: ButtonHTMLAttributes<HTMLButtonElement>) {
    const variant = className.includes('danger-button')
        ? 'danger'
        : /(?:backup|primary)-button/.test(className)
          ? 'primary'
          : 'surface'
    const surface = useGlassSurface({ kind: 'button', variant })
    return (
        <button
            {...props}
            className={`glass-button ${className}`}
            data-glass-variant={variant}
        >
            <canvas
                ref={surface}
                className="glass-surface"
                aria-hidden="true"
            />
            <span className="glass-button-label">{children}</span>
        </button>
    )
}

interface GlassSwitchProps extends Omit<
    InputHTMLAttributes<HTMLInputElement>,
    'type' | 'checked' | 'onChange'
> {
    checked: boolean
    onCheckedChange: (checked: boolean) => void
}

/** Controlled checkbox retains label/form/keyboard behavior and drag support. */
export function GlassSwitch({
    checked,
    onCheckedChange,
    ...props
}: GlassSwitchProps) {
    const surface = useGlassSurface({
        kind: 'switch',
        checked,
        setChecked: onCheckedChange,
    })
    return (
        <span className="glass-switch">
            <input
                {...props}
                type="checkbox"
                role="switch"
                checked={checked}
                onChange={(event) => onCheckedChange(event.target.checked)}
            />
            <span className="glass-switch-fallback" aria-hidden="true" />
            <canvas
                ref={surface}
                className="glass-surface"
                aria-hidden="true"
            />
        </span>
    )
}
