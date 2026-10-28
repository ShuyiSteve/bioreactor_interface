import type { LogRow } from '../types'

interface DataLogProps {
  rows: LogRow[]
}

function fmt(ts: number) {
  return new Date(ts).toLocaleTimeString([], { hour12: false })
}

function fmtNum(v: number | null | undefined, decimals: number) {
  if (v === null || v === undefined || !Number.isFinite(v)) return '—'
  return v.toFixed(decimals)
}

function StateChip({ on, label }: { on: boolean; label: string }) {
  return <span className={`chip ${on ? 'chip-on' : 'chip-off'}`}>{label}</span>
}

export default function DataLog({ rows }: DataLogProps) {
  return (
    <section className="panel log-panel">
      <div className="panel-head">
        <h2>Data log</h2>
        <span className="panel-note">Stored in SQLite</span>
      </div>
      <div className="table-scroll">
        <table className="log-table">
          <thead>
            <tr>
              <th>Time</th>
              <th className="num">Temp (°C)</th>
              <th className="num">pH</th>
              <th className="num">RPM</th>
              <th>Heater</th>
              <th>Acid</th>
              <th>Alkali</th>
              <th>Stirrer</th>
            </tr>
          </thead>
          <tbody>
            {rows.length === 0 && (
              <tr>
                <td colSpan={8} className="empty">
                  No readings yet.
                </td>
              </tr>
            )}
            {rows.map((r) => (
              <tr key={r.ts}>
                <td className="mono">{fmt(r.ts)}</td>
                <td className="num">{fmtNum(r.temperature, 2)}</td>
                <td className="num">{fmtNum(r.ph, 2)}</td>
                <td className="num">{fmtNum(r.rpm == null ? null : Math.round(r.rpm), 0)}</td>
                <td><StateChip on={!!r.heater} label={r.heater ? 'ON' : 'OFF'} /></td>
                <td><StateChip on={!!r.pump_acid} label={r.pump_acid ? 'ON' : 'OFF'} /></td>
                <td><StateChip on={!!r.pump_alkali} label={r.pump_alkali ? 'ON' : 'OFF'} /></td>
                <td><StateChip on={!!r.stirrer} label={r.stirrer ? 'ON' : 'OFF'} /></td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  )
}
