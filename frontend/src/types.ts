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

export type CommandStatus = 'pending' | 'confirmed' | 'rejected' | 'timeout'

export interface Status {
  device: { online: boolean; lastSeen: string | null }
  latest: LatestReading | null
  setpoints: Setpoints
  control: ControlState
  telemetry: { sp_source: string | null; alarms: string[] }
  setpointCommand: { id: string; status: CommandStatus; sentAt: string } | null
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
