import type { ControlState, EventRow, LogRow, ReadingsResponse, Setpoints, Status } from './types'

const BASE = (import.meta.env.VITE_API_BASE as string | undefined) ?? '/api'

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${BASE}${path}`, {
    headers: { 'Content-Type': 'application/json' },
    ...init,
  })
  if (!res.ok) {
    const text = await res.text().catch(() => '')
    throw new Error(`API ${path} failed (${res.status}): ${text}`)
  }
  return res.json() as Promise<T>
}

export const api = {
  status: () => request<Status>('/status'),
  readings: (param: string, range: string, points = 140) =>
    request<ReadingsResponse>(`/readings?param=${param}&range=${range}&points=${points}`),
  log: (limit = 100) => request<LogRow[]>(`/log?limit=${limit}`),
  events: (limit = 40) => request<EventRow[]>(`/events?limit=${limit}`),
  setpoints: (body: Partial<Setpoints>) =>
    request<Status>('/setpoints', { method: 'POST', body: JSON.stringify(body) }),
  control: (body: Partial<ControlState>) =>
    request<Status>('/control', { method: 'POST', body: JSON.stringify(body) }),
}
