export interface LatestReading {
  ts: number
  temperature: number
  ph: number
  rpm: number
  heater: number
  pump_acid: number
  pump_alkali: number
  stirrer: number
}

export type Mode = 'auto' | 'manual'

export interface Setpoints {
  temp: number
  ph: number
  rpm: number
  mode: Mode
}

export interface ControlState {
  heater: number
  pump_acid: number
  pump_alkali: number
  stirrer: number
}

export interface Status {
  device: { online: boolean; lastSeen: string | null }
  latest: LatestReading | null
  setpoints: Setpoints
  control: ControlState
}

export interface SeriesPoint {
  ts: number
  v: number
}

export interface ReadingsResponse {
  param: string
  unit: string
  rangeMs: number
  series: SeriesPoint[]
}

export interface LogRow {
  ts: number
  temperature: number
  ph: number
  rpm: number
  heater: number
  pump_acid: number
  pump_alkali: number
  stirrer: number
}

export interface EventRow {
  ts: number
  level: string
  message: string
}

export type Param = 'temp' | 'ph' | 'rpm'
