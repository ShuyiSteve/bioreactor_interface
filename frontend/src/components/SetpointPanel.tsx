import { useEffect, useState } from 'react'
import type { CSSProperties } from 'react'
import type { Setpoints, Status } from '../types'

interface SetpointPanelProps {
  setpoints: Setpoints
  command: Status['setpointCommand']
  busy: boolean
  onApply: (next: { temp: number; ph: number; rpm: number }) => void | Promise<void>
}

const CONTROLS = [
  { key: 'temp', label: 'Temperature', unit: '°C', min: 25, max: 35, step: 0.5, color: '#ff7a59' },
  { key: 'ph', label: 'pH', unit: '', min: 3, max: 7, step: 0.1, color: '#3ddc97' },
  { key: 'rpm', label: 'Stirring speed', unit: 'RPM', min: 500, max: 1500, step: 20, color: '#6ea8ff' },
] as const

const ACK_TEXT = {
  pending: { text: 'Sent, waiting for the device to confirm…', tone: 'muted' },
  confirmed: { text: '✓ Confirmed by the device', tone: 'ok' },
  rejected: { text: '⚠ Rejected by the device (out of range). Showing the values it kept.', tone: 'warn' },
  timeout: { text: '⚠ No confirmation from the device within 5 s. Check the link.', tone: 'warn' },
} as const

type Key = (typeof CONTROLS)[number]['key']

export default function SetpointPanel({ setpoints, command, busy, onApply }: SetpointPanelProps) {
  const [draft, setDraft] = useState<Record<Key, number>>({
    temp: setpoints.temp,
    ph: setpoints.ph,
    rpm: setpoints.rpm,
  })

  useEffect(() => {
    setDraft({ temp: setpoints.temp, ph: setpoints.ph, rpm: setpoints.rpm })
  }, [setpoints.temp, setpoints.ph, setpoints.rpm])

  const dirty =
    draft.temp !== setpoints.temp ||
    draft.ph !== setpoints.ph ||
    draft.rpm !== setpoints.rpm

  return (
    <section className="panel">
      <div className="panel-head">
        <h2>Set points</h2>
        <span className="panel-note">Values are clamped to the specification ranges</span>
      </div>

      <div className="setpoint-list">
        {CONTROLS.map((c) => {
          const value = draft[c.key]
          const pct = ((value - c.min) / (c.max - c.min)) * 100
          return (
            <div className="setpoint-row" key={c.key}>
              <div className="setpoint-top">
                <span className="setpoint-name" style={{ color: c.color }}>
                  {c.label}
                </span>
                <input
                  className="setpoint-num"
                  type="number"
                  value={value}
                  min={c.min}
                  max={c.max}
                  step={c.step}
                  onChange={(e) => {
                    const n = Number(e.target.value)
                    if (Number.isFinite(n)) setDraft((d) => ({ ...d, [c.key]: n }))
                  }}
                />
                <span className="setpoint-unit">{c.unit}</span>
              </div>
              <input
                className="setpoint-slider"
                type="range"
                style={{ '--fill': `${pct}%`, '--accent': c.color } as CSSProperties}
                min={c.min}
                max={c.max}
                step={c.step}
                value={value}
                onChange={(e) =>
                  setDraft((d) => ({ ...d, [c.key]: Number(e.target.value) }))
                }
              />
              <div className="setpoint-range">
                <span>{c.min}</span>
                <span>{c.max}</span>
              </div>
            </div>
          )
        })}
      </div>

      {command && (
        <p className={`ack-status ack-${ACK_TEXT[command.status].tone}`} role="status">
          {ACK_TEXT[command.status].text}
        </p>
      )}

      <div className="panel-actions">
        <button
          type="button"
          className="btn primary"
          disabled={!dirty || busy}
          onClick={() => onApply({ ...draft })}
        >
          {busy ? 'Applying…' : 'Apply set points'}
        </button>
        <button
          type="button"
          className="btn ghost"
          disabled={!dirty || busy}
          onClick={() =>
            setDraft({ temp: setpoints.temp, ph: setpoints.ph, rpm: setpoints.rpm })
          }
        >
          Reset
        </button>
      </div>
    </section>
  )
}
