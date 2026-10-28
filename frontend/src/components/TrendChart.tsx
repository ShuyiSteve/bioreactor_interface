import { useMemo } from 'react'
import type { SeriesPoint } from '../types'

interface TrendChartProps {
  data: SeriesPoint[]
  color: string
  unit: string
  decimals: number
  domain?: [number, number]
  reference?: number
  band?: number
}

const W = 640
const H = 240
const PAD = { top: 16, right: 18, bottom: 30, left: 56 }

function fmtValue(v: number, decimals: number) {
  return v.toFixed(decimals)
}

function fmtTime(t: number) {
  return new Date(t).toLocaleTimeString([], { hour12: false })
}

export default function TrendChart({
  data,
  color,
  unit,
  decimals,
  domain,
  reference,
  band,
}: TrendChartProps) {
  const geom = useMemo(() => {
    const innerW = W - PAD.left - PAD.right
    const innerH = H - PAD.top - PAD.bottom

    let min: number
    let max: number
    if (domain) {
      ;[min, max] = domain
    } else if (data.length === 0) {
      min = 0
      max = 1
    } else {
      const values = data.map((d) => d.v)
      min = Math.min(...values)
      max = Math.max(...values)
      if (min === max) {
        min -= 1
        max += 1
      } else {
        const pad = (max - min) * 0.12
        min -= pad
        max += pad
      }
    }

    const x = (i: number) =>
      PAD.left + (data.length <= 1 ? innerW / 2 : (i / (data.length - 1)) * innerW)
    const y = (v: number) => PAD.top + (1 - (v - min) / (max - min)) * innerH

    const line = data.map((d, i) => `${i === 0 ? 'M' : 'L'} ${x(i).toFixed(1)} ${y(d.v).toFixed(1)}`).join(' ')
    const area = `${line} L ${x(data.length - 1).toFixed(1)} ${(PAD.top + innerH).toFixed(1)} L ${PAD.left} ${(PAD.top + innerH).toFixed(1)} Z`

    const ticks = [0, 1 / 3, 2 / 3, 1].map((f) => ({
      y: PAD.top + innerH * f,
      label: fmtValue(min + (max - min) * (1 - f), decimals),
    }))

    const refY = reference !== undefined ? y(reference) : undefined
    const bandTop = refY !== undefined && band !== undefined ? y(reference + band) : undefined
    const bandBottom = refY !== undefined && band !== undefined ? y(reference - band) : undefined

    return { innerW, innerH, min, max, line, area, ticks, refY, bandTop, bandBottom, x, y }
  }, [data, domain, decimals, reference, band])

  const last = data[data.length - 1]
  const gradId = `grad-${color.replace('#', '')}`

  return (
    <div className="chart-wrap">
      <svg viewBox={`0 0 ${W} ${H}`} className="chart" role="img" aria-label="Trend chart">
        <defs>
          <linearGradient id={gradId} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stopColor={color} stopOpacity="0.28" />
            <stop offset="100%" stopColor={color} stopOpacity="0.02" />
          </linearGradient>
        </defs>

        {geom.ticks.map((t) => (
          <g key={t.y}>
            <line
              x1={PAD.left}
              x2={W - PAD.right}
              y1={t.y}
              y2={t.y}
              className="chart-grid"
            />
            <text x={PAD.left - 8} y={t.y + 4} className="chart-axis" textAnchor="end">
              {t.label}
            </text>
          </g>
        ))}

        {geom.bandTop !== undefined && geom.bandBottom !== undefined && (
          <rect
            x={PAD.left}
            y={geom.bandTop}
            width={geom.innerW}
            height={Math.max(0, geom.bandBottom - geom.bandTop)}
            className="chart-band"
          />
        )}

        {geom.refY !== undefined && (
          <line
            x1={PAD.left}
            x2={W - PAD.right}
            y1={geom.refY}
            y2={geom.refY}
            className="chart-ref"
          />
        )}

        <path d={geom.area} fill={`url(#${gradId})`} />
        <path d={geom.line} fill="none" stroke={color} strokeWidth="2.2" strokeLinejoin="round" />

        {last && (
          <g>
            <circle
              cx={geom.x(data.length - 1)}
              cy={geom.y(last.v)}
              r="4"
              fill={color}
              stroke="#0b1220"
              strokeWidth="2"
            />
            <text
              x={Math.min(geom.x(data.length - 1), W - PAD.right - 58)}
              y={geom.y(last.v) - 10}
              className="chart-last"
            >
              {fmtValue(last.v, decimals)}
              {unit}
            </text>
          </g>
        )}

        {data.length > 1 && (
          <g className="chart-axis">
            <text x={PAD.left} y={H - 8}>
              {fmtTime(data[0].ts)}
            </text>
            <text x={W - PAD.right} y={H - 8} textAnchor="end">
              {fmtTime(last.ts)}
            </text>
          </g>
        )}
      </svg>
      {data.length === 0 && <div className="chart-empty">Waiting for data…</div>}
    </div>
  )
}
