// Bioreactor control server.
// - REST API for the web dashboard.
// - Ingestion endpoint for the ESP32 (two-way: returns pending commands).
// - Optional built-in simulator so the UI is "live" without hardware.
//
// Environment variables:
//   PORT      HTTP port (default 3001)
//   DB_PATH   SQLite file location (default backend/bioreactor.db)
//   SIMULATE  "true" (default) to generate live demo data; "false" to disable
//   SEED      "true" (default) to seed demo history if the DB is empty
//   MQTT_URL      MQTT broker URL, e.g. "mqtt://broker.hivemq.com:1883" (disabled if unset)
//   MQTT_TOPIC    MQTT topic prefix (default "bioreactor")
//   MQTT_DEVICE_ID device id for outgoing MQTT commands (default "bio1")
'use strict';

process.env.NODE_NO_WARNINGS = process.env.NODE_NO_WARNINGS || '1';

const path = require('node:path');
const fs = require('node:fs');
const express = require('express');
const cors = require('cors');
const db = require('./db');
const mqttBridge = require('./mqtt');
const { generateDemoData } = require('./seed');

const PORT = Number(process.env.PORT) || 3001;
const SIMULATE = String(process.env.SIMULATE ?? 'true') !== 'false';
const SEED = String(process.env.SEED ?? 'true') !== 'false';
const ONLINE_WINDOW_MS = 15000;
const MQTT_DEVICE_ID = process.env.MQTT_DEVICE_ID || 'bio1';

// Device presence from the MQTT status topic (item 4): the DevKitC publishes
// "online" on connect and the broker publishes "offline" via its last will when
// the link drops. Keyed by device id (usually "bio1").
const deviceStatus = new Map(); // device -> { status: 'online'|'offline', at: ms }

function markDeviceOnline(device) {
  deviceStatus.set(String(device), { status: 'online', at: Date.now() });
}

function setDeviceStatus(device, statusText) {
  const status = String(statusText || '').trim().toLowerCase();
  if (status !== 'online' && status !== 'offline') return;
  const key = String(device);
  const previous = deviceStatus.get(key);
  deviceStatus.set(key, { status, at: Date.now() });
  if (!previous || previous.status !== status) {
    db.logEvent('info', `Device ${key} is ${status}`);
    console.log(`[mqtt] ${key} -> ${status}`);
  }
}

const app = express();
app.use(cors());
app.use(express.json({ limit: '1mb' }));

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function parseRange(range) {
  const m = /^(\d+)([smhd])$/.exec(String(range || '10m'));
  if (!m) return 10 * 60 * 1000;
  const n = Number(m[1]);
  const mult = { s: 1000, m: 60_000, h: 3_600_000, d: 86_400_000 }[m[2]];
  return n * mult;
}

function buildStatus() {
  const latest = db.getLatest();
  const setpoints = db.getSettings();
  const control = db.getControlState();
  const lastSeen = latest ? latest.ts : 0;
  const st = deviceStatus.get(MQTT_DEVICE_ID);
  // Item 4: the MQTT status topic is authoritative when we have it; otherwise
  // fall back to "recent reading" so simulation/HTTP mode still works.
  const online = st
    ? st.status === 'online'
    : Date.now() - lastSeen < ONLINE_WINDOW_MS;
  return {
    device: {
      online,
      lastSeen: lastSeen ? new Date(lastSeen).toISOString() : null,
    },
    latest: latest || null,
    setpoints,
    control,
  };
}

function meanOf(v) {
  if (v === null || v === undefined) return null;
  if (typeof v === 'number') return v;
  return db.num(v.mean, null);
}

// Accepts BOTH the course telemetry shape (mean/min/max + actuators_avg) and
// the legacy flat shape, and stores a single row per sample.
function storeTelemetry(msg) {
  if (!msg) return null;
  const isNew =
    msg.temperature_C !== undefined ||
    (msg.pH && msg.pH.mean !== undefined) ||
    (msg.rpm && msg.rpm.mean !== undefined);

  let temperature, ph, rpm, heater, acid, alkali, stirrer;
  let seq = null, device = null, ts = Date.now();

  if (isNew) {
    temperature = meanOf(msg.temperature_C);
    ph = meanOf(msg.pH);
    rpm = meanOf(msg.rpm);
    const act = msg.actuators_avg || {};
    heater = db.num(act.heater_pwm, 0) > 0.05 ? 1 : 0;
    acid = db.num(act.acid_pwm, 0) > 0.05 ? 1 : 0;
    alkali = db.num(act.base_pwm, 0) > 0.05 ? 1 : 0;
    stirrer = db.num(act.motor_pwm, 0) > 0.05 ? 1 : 0;
    seq = msg.seq === undefined ? null : Number(msg.seq);
    device = msg.device === undefined ? null : String(msg.device);
    ts = db.num(msg.ts, Date.now());
  } else {
    temperature = db.num(msg.temperature);
    ph = db.num(msg.ph);
    rpm = db.num(msg.rpm);
    heater = msg.heater ? 1 : 0;
    acid = msg.pump_acid ? 1 : 0;
    alkali = msg.pump_alkali ? 1 : 0;
    stirrer = msg.stirrer === undefined ? 1 : msg.stirrer ? 1 : 0;
    device = msg.device === undefined ? null : String(msg.device);
    ts = db.num(msg.ts, Date.now());
  }

  return db.insertReading({
    ts,
    seq,
    device,
    temperature,
    ph,
    rpm,
    heater,
    pump_acid: acid,
    pump_alkali: alkali,
    stirrer,
    raw: msg,
  });
}

// ---------------------------------------------------------------------------
// API routes
// ---------------------------------------------------------------------------

app.get('/api/health', (_req, res) => {
  res.json({ ok: true, simulate: SIMULATE, db: db.DB_PATH });
});

// Latest values, setpoints and actuator states (one call for the dashboard).
app.get('/api/status', (_req, res) => {
  res.json(buildStatus());
});

// Time-series for a parameter: /api/readings?param=temp|ph|rpm&range=10m&points=120
app.get('/api/readings', (req, res) => {
  try {
    const param = String(req.query.param || 'temp');
    const unit = { temp: '°C', ph: 'pH', rpm: 'RPM' }[param];
    if (!unit) return res.status(400).json({ error: 'param must be temp, ph or rpm' });
    const rangeMs = parseRange(req.query.range);
    const series = db.getSeries(param, rangeMs, Number(req.query.points) || 120);
    res.json({ param, unit, rangeMs, series });
  } catch (err) {
    res.status(500).json({ error: String(err) });
  }
});

// Recent readings for the data table.
app.get('/api/log', (req, res) => {
  res.json(db.getLog(req.query.limit));
});

// Recent activity feed.
app.get('/api/events', (req, res) => {
  res.json(db.getEvents(req.query.limit));
});

// Update setpoints. Body: { temp?, ph?, rpm?, mode? } — values are clamped to spec.
app.post('/api/setpoints', (req, res) => {
  const body = req.body || {};
  const cur = db.getSettings();
  const next = {
    temp: db.clamp(body.temp, 25, 35, cur.temp),
    ph: db.clamp(body.ph, 3, 7, cur.ph),
    rpm: db.clamp(body.rpm, 500, 1500, cur.rpm),
    mode: body.mode === 'manual' || body.mode === 'auto' ? body.mode : cur.mode,
  };
  db.updateSettings(next);
  const setpointPayload = { temperature_C: next.temp, pH: next.ph, rpm: next.rpm, mode: next.mode };
  const setpointCmd = db.enqueueCommand('setpoints', setpointPayload);
  mqttBridge.publishCommand(setpointCmd.id, 'setpoints', setpointPayload);
  db.logEvent(
    'info',
    `Setpoints → ${next.temp.toFixed(1)} °C · pH ${next.ph.toFixed(1)} · ${Math.round(next.rpm)} RPM · ${next.mode}`
  );
  res.json(buildStatus());
});

// Manual actuator override. Body: { heater?, pump_acid?, pump_alkali?, stirrer? }
app.post('/api/control', (req, res) => {
  const body = req.body || {};
  const cur = db.getControlState();
  const next = {
    heater: db.bool(body.heater, cur.heater),
    pump_acid: db.bool(body.pump_acid, cur.pump_acid),
    pump_alkali: db.bool(body.pump_alkali, cur.pump_alkali),
    stirrer: db.bool(body.stirrer, cur.stirrer),
  };
  db.updateControlState(next);
  const controlPayload = {
    heater_pwm: next.heater ? 0.6 : 0.0,   // 60% safety cap (30 W heater)
    motor_pwm: next.stirrer ? 1.0 : 0.0,
    acid_pwm: next.pump_acid ? 1.0 : 0.0,
    base_pwm: next.pump_alkali ? 1.0 : 0.0,
  };
  const controlCmd = db.enqueueCommand('control', controlPayload);
  mqttBridge.publishCommand(controlCmd.id, 'control', controlPayload);
  db.logEvent(
    'info',
    `Manual control → heater ${next.heater ? 'ON' : 'OFF'} · acid ${next.pump_acid ? 'ON' : 'OFF'} · alkali ${next.pump_alkali ? 'ON' : 'OFF'} · stirrer ${next.stirrer ? 'ON' : 'OFF'}`
  );
  res.json(buildStatus());
});

// ESP32 pushes a reading here and receives any pending commands in the response.
app.post('/api/ingest', (req, res) => {
  storeTelemetry(req.body || {});
  const commands = db.getPendingCommands(true);
  res.json({ ok: true, commands });
});

// ESP32 polls for pending commands (alternative to reading them off /api/ingest).
app.get('/api/commands/pending', (_req, res) => {
  res.json({ commands: db.getPendingCommands(true) });
});

// ESP32 acknowledges receipt of commands.
app.post('/api/commands/ack', (req, res) => {
  const ids = (req.body || {}).ids;
  const changed = db.ackCommands(ids);
  res.json({ ok: true, acked: changed });
});

// ---------------------------------------------------------------------------
// Built-in simulator (demo mode)
// ---------------------------------------------------------------------------

const SIM_TICK_MS = 500;             // simulator update interval (0.5 s)
const SIM_DT = SIM_TICK_MS / 2500;     // time-step scale: keeps dynamics the same at 1 s
const SIM_SAMPLES = 10;                // simulated samples per telemetry window

const sim = { temperature: 28.6, ph: 5.1, rpm: 1004 };
let simSeq = 0;

function simulateTick() {
  const s = db.getSettings();
  const c = db.getControlState();
  const auto = s.mode === 'auto';

  let heaterOn = !!c.heater;
  let acidOn = !!c.pump_acid;
  let alkaliOn = !!c.pump_alkali;
  let stirrerOn = !!c.stirrer;

  if (auto) {
    // Hysteresis controller (the real logic runs on the ESP32).
    if (sim.temperature < s.temp - 0.25) heaterOn = true;
    else if (sim.temperature > s.temp + 0.25) heaterOn = false;
    acidOn = sim.ph > s.ph + 0.1;
    alkaliOn = sim.ph < s.ph - 0.1;
    stirrerOn = true;
    db.updateControlState({
      heater: heaterOn ? 1 : 0,
      pump_acid: acidOn ? 1 : 0,
      pump_alkali: alkaliOn ? 1 : 0,
      stirrer: stirrerOn ? 1 : 0,
    });
  }

  // Simple first-order plant model + noise.
  const heatTarget = heaterOn ? s.temp + 0.8 : 18.5;
  sim.temperature += (heatTarget - sim.temperature) * 0.06 * SIM_DT + (Math.random() - 0.5) * 0.12 * Math.sqrt(SIM_DT);

  if (acidOn) sim.ph -= 0.008 * SIM_DT;
  if (alkaliOn) sim.ph += 0.008 * SIM_DT;
  sim.ph += (s.ph - sim.ph) * 0.02 * SIM_DT + (Math.random() - 0.5) * 0.02 * Math.sqrt(SIM_DT);
  sim.ph = Math.min(7.5, Math.max(2.5, sim.ph));

  const rpmTarget = stirrerOn ? s.rpm : 0;
  sim.rpm += (rpmTarget - sim.rpm) * 0.15 * SIM_DT + (Math.random() - 0.5) * 6 * Math.sqrt(SIM_DT);

  // Actuator duties (0..1). Heater is PWM-capped at 60% (item 1).
  const heaterDuty = heaterOn ? 0.6 : 0.0;
  const motorDuty = stirrerOn ? db.clamp((s.rpm - 400) / 1100, 0, 1, 0.5) : 0.0;
  const acidDuty = acidOn ? 1.0 : 0.0;
  const baseDuty = alkaliOn ? 1.0 : 0.0;

  const r2 = (v) => Math.round(v * 100) / 100;
  const now = Date.now();

  // Emit the SAME course telemetry shape the Nano sends, so Data Analysis
  // works identically on simulated and real data.
  storeTelemetry({
    device: MQTT_DEVICE_ID,
    seq: ++simSeq,
    ts: now,
    window: { start: now - SIM_TICK_MS, end: now, seconds: SIM_TICK_MS / 1000, samples: SIM_SAMPLES },
    temperature_C: { mean: r2(sim.temperature), min: r2(sim.temperature - 0.08), max: r2(sim.temperature + 0.08) },
    pH: { mean: r2(sim.ph), min: r2(sim.ph - 0.02), max: r2(sim.ph + 0.02) },
    rpm: { mean: Math.max(0, Math.round(sim.rpm)), min: Math.max(0, Math.round(sim.rpm - 4)), max: Math.max(0, Math.round(sim.rpm + 4)) },
    actuators_avg: { heater_pwm: heaterDuty, motor_pwm: motorDuty, acid_pwm: acidDuty, base_pwm: baseDuty },
    setpoints: { temperature_C: s.temp, pH: s.ph, rpm: s.rpm },
    mode: s.mode,
    sp_source: 'dashboard',
    alarms: [],
  });
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

if (SEED && db.countReadings() === 0) {
  generateDemoData();
  console.log('Seeded demo history into', db.DB_PATH);
}

if (SIMULATE) {
  simulateTick();
  setInterval(simulateTick, SIM_TICK_MS);
  console.log(`Simulator enabled: generating live readings every ${SIM_TICK_MS / 1000}s.`);
}

// Serve the built frontend if it exists (production mode).
const dist = path.join(__dirname, '..', 'frontend', 'dist');
if (fs.existsSync(dist)) {
  app.use(express.static(dist));
  app.use((req, res, next) => {
    if (req.method === 'GET' && !req.path.startsWith('/api')) {
      return res.sendFile(path.join(dist, 'index.html'));
    }
    next();
  });
}

// Optional MQTT ingress/egress (per the Connectivity spec).
mqttBridge.start({
  url: process.env.MQTT_URL || '',
  topicPrefix: process.env.MQTT_TOPIC || 'bioreactor',
  commandDeviceId: process.env.MQTT_DEVICE_ID || 'bio1',
  onTelemetry: (device, msg) => {
    storeTelemetry(msg);
    if (device) markDeviceOnline(device);   // live data proves the link is up
  },
  onAck: (_device, msg) => {
    const n = db.ackCommand(msg.id, msg.ok === true);
    if (n > 0) db.logEvent('info', `Command ${msg.id} acked (ok=${msg.ok})`);
  },
  onStatus: (device, statusText) => {
    setDeviceStatus(device, statusText);
  },
});

app.listen(PORT, () => {
  console.log(`Bioreactor server listening on http://localhost:${PORT}`);
  console.log(`SQLite database: ${db.DB_PATH}`);
  if (mqttBridge.isEnabled()) {
    console.log(`MQTT bridge: ${process.env.MQTT_URL} (topic "${process.env.MQTT_TOPIC || 'bioreactor'}")`);
  }
});
