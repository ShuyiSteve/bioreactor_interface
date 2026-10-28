import type { CSSProperties, ReactNode } from 'react'

interface StatCardProps {
  label: string
  value: string
  unit: string
  accent: string
  setpoint: string
  deviation: string
  tone: 'ok' | 'warn'
  actuators?: ReactNode
}

export default function StatCard({
  label,
  value,
  unit,
  accent,
  setpoint,
  deviation,
  tone,
  actuators,
}: StatCardProps) {
  return (
    <div className="stat-card" style={{ '--accent': accent } as CSSProperties}>
      <div className="stat-head">
        <span className="stat-label">{label}</span>
        <span className={`stat-deviation ${tone}`}>{deviation}</span>
      </div>
      <div className="stat-value">
        {value}
        <span className="stat-unit">{unit}</span>
      </div>
      <div className="stat-setpoint">Set point {setpoint}</div>
      {actuators && <div className="stat-actuators">{actuators}</div>}
    </div>
  )
}
