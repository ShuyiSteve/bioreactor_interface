import type { ControlState, Mode } from '../types'

interface ControlPanelProps {
  control: ControlState
  mode: Mode
  busy: boolean
  onToggle: (key: keyof ControlState, value: boolean) => void
  onModeChange: (mode: Mode) => void
}

const TOGGLES = [
  { key: 'heater', label: 'Heater', desc: '3 Ω · 30 W heating element', kind: 'heater' },
  { key: 'stirrer', label: 'Stirrer', desc: '500–1500 RPM motor', kind: 'stirrer' },
  { key: 'pump_acid', label: 'Acid pump', desc: '6 V peristaltic pump', kind: 'acid' },
  { key: 'pump_alkali', label: 'Alkali pump', desc: '6 V peristaltic pump', kind: 'alkali' },
] as const

export default function ControlPanel({
  control,
  mode,
  busy,
  onToggle,
  onModeChange,
}: ControlPanelProps) {
  const auto = mode === 'auto'
  return (
    <section className="panel">
      <div className="panel-head">
        <h2>Actuators</h2>
        <div className="mode-switch" role="group" aria-label="Control mode">
          <button
            type="button"
            className={`mode-btn ${auto ? 'active' : ''}`}
            onClick={() => onModeChange('auto')}
            disabled={busy}
          >
            Auto
          </button>
          <button
            type="button"
            className={`mode-btn ${!auto ? 'active' : ''}`}
            onClick={() => onModeChange('manual')}
            disabled={busy}
          >
            Manual
          </button>
        </div>
      </div>

      {auto ? (
        <p className="panel-note auto-note">
          Automatic mode — the controller on the ESP32 drives the actuators to hold the
          set points. Switch to Manual to override.
        </p>
      ) : (
        <p className="panel-note">Manual mode — you are overriding the controller.</p>
      )}

      <div className="toggle-list">
        {TOGGLES.map((t) => {
          const on = !!control[t.key]
          return (
            <div className="toggle-row" key={t.key}>
              <div className="toggle-meta">
                <span className="toggle-label">{t.label}</span>
                <span className="toggle-desc">{t.desc}</span>
              </div>
              <button
                type="button"
                role="switch"
                aria-checked={on}
                aria-label={`Toggle ${t.label}`}
                className={`switch ${t.kind} ${on ? 'on' : ''}`}
                disabled={auto || busy}
                onClick={() => onToggle(t.key, !on)}
              >
                <span className="switch-knob" />
              </button>
            </div>
          )
        })}
      </div>
    </section>
  )
}
