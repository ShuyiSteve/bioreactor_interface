import { useEffect, useRef, useState } from 'react'
import type { CSSProperties } from 'react'
import { api } from './api'
import type {
  ControlState,
  EventRow,
  LogRow,
  Param,
  SeriesPoint,
  Status,
} from './types'
import StatCard from './components/StatCard'
import ActuatorTag from './components/ActuatorTag'
import TrendChart from './components/TrendChart'
import SetpointPanel from './components/SetpointPanel'
import ControlPanel from './components/ControlPanel'
import DataLog from './components/DataLog'

const PARAM_META: Record<
  Param,
  { label: string; color: string; chartUnit: string; decimals: number; domain: [number, number]; band: number }
> = {
  temp: { label: 'Temperature', color: '#ff7a59', chartUnit: '°C', decimals: 1, domain: [18, 38], band: 0.5 },
  ph: { label: 'pH', color: '#3ddc97', chartUnit: '', decimals: 2, domain: [3, 7], band: 0.2 },
  rpm: { label: 'Stirring speed', color: '#6ea8ff', chartUnit: ' RPM', decimals: 0, domain: [0, 1600], band: 20 },
}

const RANGES = ['5m', '15m', '1h', '6h'] as const

function usePoll<T>(
  fn: () => Promise<T>,
  intervalMs: number,
  deps: ReadonlyArray<unknown>,
): { value: T | null; error: string | null } {
  const [value, setValue] = useState<T | null>(null)
  const [error, setError] = useState<string | null>(null)
  const fnRef = useRef(fn)
  useEffect(() => {
    fnRef.current = fn
  })
  useEffect(() => {
    let active = true
    const run = async () => {
      try {
        const v = await fnRef.current()
        if (active) {
          setValue(v)
          setError(null)
        }
      } catch (e) {
        if (active) setError(e instanceof Error ? e.message : String(e))
      }
    }
    run()
    const id = setInterval(run, intervalMs)
    return () => {
      active = false
      clearInterval(id)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [intervalMs, ...deps])
  return { value, error }
}

function fmtVal(v: number | null | undefined, decimals: number) {
  if (v === null || v === undefined || !Number.isFinite(v)) return '—'
  return v.toFixed(decimals)
}

function deviationText(
  actual: number | null | undefined,
  target: number,
  decimals: number,
  unit: string,
  tolerance: number,
) {
  if (actual === null || actual === undefined) return { text: '—', ok: true }
  const d = Math.abs(actual - target)
  return { text: `± ${d.toFixed(decimals)} ${unit}`.trim(), ok: d <= tolerance }
}

export default function App() {
  const [param, setParam] = useState<Param>('temp')
  const [range, setRange] = useState<string>('15m')
  const [busy, setBusy] = useState(false)

  const statusRes = usePoll(() => api.status(), 2000, [])
  const seriesRes = usePoll(() => api.readings(param, range, 140), 4000, [param, range])
  const logRes = usePoll(() => api.log(120), 8000, [])
  const eventsRes = usePoll(() => api.events(30), 8000, [])

  const status: Status | null = statusRes.value
  const series: SeriesPoint[] = seriesRes.value?.series ?? []
  const log: LogRow[] = logRes.value ?? []
  const events: EventRow[] = eventsRes.value ?? []
  const error = statusRes.error ?? seriesRes.error

  const latest = status?.latest ?? null
  const setpoints = status?.setpoints
  const control: ControlState | undefined = status?.control
  const online = status?.device.online ?? false

  const meta = PARAM_META[param]
  const reference = setpoints ? setpoints[param] : undefined

  const applySetpoints = async (next: { temp: number; ph: number; rpm: number }) => {
    setBusy(true)
    try {
      await api.setpoints(next)
    } finally {
      setBusy(false)
    }
  }

  const toggleControl = async (key: keyof ControlState, value: boolean) => {
    setBusy(true)
    try {
      await api.control({ [key]: value ? 1 : 0 })
    } finally {
      setBusy(false)
    }
  }

  const changeMode = async (mode: 'auto' | 'manual') => {
    setBusy(true)
    try {
      await api.setpoints({ mode })
    } finally {
      setBusy(false)
    }
  }

  const tempDev = deviationText(latest?.temperature, setpoints?.temp ?? 30, 1, '°C', 0.5)
  const phDev = deviationText(latest?.ph, setpoints?.ph ?? 5, 2, '', 0.2)
  const rpmDev = deviationText(latest?.rpm, setpoints?.rpm ?? 1000, 0, 'RPM', 20)

  const lastSeen = status?.device.lastSeen
    ? new Date(status.device.lastSeen).toLocaleTimeString([], { hour12: false })
    : null

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <span className="brand-dot" />
          <h1>Bioreactor Control</h1>
        </div>
        <div className="topbar-right">
          <span className={`conn ${online ? 'conn-ok' : 'conn-off'}`}>
            <i className="conn-dot" />
            {online ? 'Device connected' : 'Device offline'}
          </span>
          {lastSeen && <span className="conn-meta">last seen {lastSeen}</span>}
        </div>
      </header>

      <main className="layout">
        {error && <div className="banner error">⚠ {error}</div>}

        <section className="stats-grid">
          <StatCard
            label="Temperature"
            value={fmtVal(latest?.temperature, 1)}
            unit="°C"
            accent={PARAM_META.temp.color}
            setpoint={`${(setpoints?.temp ?? 30).toFixed(1)} °C`}
            deviation={tempDev.text}
            tone={tempDev.ok ? 'ok' : 'warn'}
            actuators={
              <ActuatorTag label="Heater" on={!!control?.heater} kind="heater" />
            }
          />
          <StatCard
            label="pH"
            value={fmtVal(latest?.ph, 2)}
            unit=""
            accent={PARAM_META.ph.color}
            setpoint={`pH ${(setpoints?.ph ?? 5).toFixed(1)}`}
            deviation={phDev.text}
            tone={phDev.ok ? 'ok' : 'warn'}
            actuators={
              <>
                <ActuatorTag label="Acid" on={!!control?.pump_acid} kind="acid" />
                <ActuatorTag label="Alkali" on={!!control?.pump_alkali} kind="alkali" />
              </>
            }
          />
          <StatCard
            label="Stirring speed"
            value={fmtVal(latest?.rpm == null ? null : Math.round(latest.rpm), 0)}
            unit="RPM"
            accent={PARAM_META.rpm.color}
            setpoint={`${Math.round(setpoints?.rpm ?? 1000)} RPM`}
            deviation={rpmDev.text}
            tone={rpmDev.ok ? 'ok' : 'warn'}
            actuators={
              <ActuatorTag label="Stirrer" on={!!control?.stirrer} kind="stirrer" />
            }
          />
        </section>

        <section className="panel chart-panel">
          <div className="chart-head">
            <div className="tabs">
              {(Object.keys(PARAM_META) as Param[]).map((p) => (
                <button
                  key={p}
                  type="button"
                  className={`tab ${p === param ? 'active' : ''}`}
                  style={p === param ? { '--tab': PARAM_META[p].color } as CSSProperties : undefined}
                  onClick={() => setParam(p)}
                >
                  {PARAM_META[p].label}
                </button>
              ))}
            </div>
            <div className="range-tabs">
              {RANGES.map((r) => (
                <button
                  key={r}
                  type="button"
                  className={`range-tab ${r === range ? 'active' : ''}`}
                  onClick={() => setRange(r)}
                >
                  {r}
                </button>
              ))}
            </div>
          </div>
          <TrendChart
            data={series}
            color={meta.color}
            unit={meta.chartUnit}
            decimals={meta.decimals}
            domain={meta.domain}
            reference={reference}
            band={meta.band}
          />
        </section>

        <div className="side-grid">
          {setpoints && (
            <SetpointPanel setpoints={setpoints} busy={busy} onApply={applySetpoints} />
          )}
          {control && setpoints && (
            <ControlPanel
              control={control}
              mode={setpoints.mode}
              busy={busy}
              onToggle={toggleControl}
              onModeChange={changeMode}
            />
          )}
        </div>

        <DataLog rows={log} />

        <section className="panel events-panel">
          <div className="panel-head">
            <h2>Activity</h2>
            <span className="panel-note">Recent events</span>
          </div>
          <ul className="events-list">
            {events.length === 0 && <li className="empty">No events yet.</li>}
            {events.map((ev) => (
              <li key={`${ev.ts}-${ev.message}`} className="event-item">
                <span className="mono event-time">
                  {new Date(ev.ts).toLocaleTimeString([], { hour12: false })}
                </span>
                <span className="event-msg">{ev.message}</span>
              </li>
            ))}
          </ul>
        </section>
      </main>
    </div>
  )
}
