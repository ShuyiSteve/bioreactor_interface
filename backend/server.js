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
  const online = Date.now() - lastSeen < ONLINE_WINDOW_MS;
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
  db.enqueueCommand('setpoints', next);
  mqttBridge.publishCommand('setpoints', next);
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
  db.enqueueCommand('control', next);
  mqttBridge.publishCommand('control', next);
  db.logEvent(
    'info',
    `Manual control → heater ${next.heater ? 'ON' : 'OFF'} · acid ${next.pump_acid ? 'ON' : 'OFF'} · alkali ${next.pump_alkali ? 'ON' : 'OFF'} · stirrer ${next.stirrer ? 'ON' : 'OFF'}`
  );
  res.json(buildStatus());
});

// ESP32 pushes a reading here and receives any pending commands in the response.
app.post('/api/ingest', (req, res) => {
  const b = req.body || {};
  db.insertReading({
    temperature: db.num(b.temperature),
    ph: db.num(b.ph),
    rpm: db.num(b.rpm),
    heater: b.heater,
    pump_acid: b.pump_acid,
    pump_alkali: b.pump_alkali,
    stirrer: b.stirrer === undefined ? 1 : b.stirrer,
  });
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

const sim = { temperature: 28.6, ph: 5.1, rpm: 1004, heater: 0 };

function simulateTick() {
  const s = db.getSettings();
  const c = db.getControlState();
  const auto = s.mode === 'auto';

  let heater = c.heater;
  let acid = c.pump_acid;
  let alkali = c.pump_alkali;
  let stirrer = c.stirrer;

  if (auto) {
    // Hysteresis controller (the real logic would run on the ESP32).
    if (sim.temperature < s.temp - 0.25) heater = 1;
    else if (sim.temperature > s.temp + 0.25) heater = 0;
    acid = sim.ph > s.ph + 0.1 ? 1 : 0;
    alkali = sim.ph < s.ph - 0.1 ? 1 : 0;
    stirrer = 1;
    db.updateControlState({
      heater,
      pump_acid: acid,
      pump_alkali: alkali,
      stirrer,
    });
  }

  // Simple first-order plant model + noise.
  const heatTarget = heater ? s.temp + 0.8 : 18.5;
  sim.temperature += (heatTarget - sim.temperature) * 0.06 + (Math.random() - 0.5) * 0.12;

  if (acid) sim.ph -= 0.008;
  if (alkali) sim.ph += 0.008;
  sim.ph += (s.ph - sim.ph) * 0.02 + (Math.random() - 0.5) * 0.02;
  sim.ph = Math.min(7.5, Math.max(2.5, sim.ph));

  const rpmTarget = stirrer ? s.rpm : 0;
  sim.rpm += (rpmTarget - sim.rpm) * 0.15 + (Math.random() - 0.5) * 6;

  db.insertReading({
    temperature: Math.round(sim.temperature * 100) / 100,
    ph: Math.round(sim.ph * 100) / 100,
    rpm: Math.max(0, Math.round(sim.rpm)),
    heater,
    pump_acid: acid,
    pump_alkali: alkali,
    stirrer,
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
  setInterval(simulateTick, 2500);
  console.log('Simulator enabled: generating live readings every 2.5s.');
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
  onTelemetry: (_device, msg) => {
    db.insertReading({
      temperature: db.num(msg.temperature),
      ph: db.num(msg.ph),
      rpm: db.num(msg.rpm),
      heater: msg.heater,
      pump_acid: msg.pump_acid,
      pump_alkali: msg.pump_alkali,
      stirrer: msg.stirrer === undefined ? 1 : msg.stirrer,
    });
  },
});

app.listen(PORT, () => {
  console.log(`Bioreactor server listening on http://localhost:${PORT}`);
  console.log(`SQLite database: ${db.DB_PATH}`);
  if (mqttBridge.isEnabled()) {
    console.log(`MQTT bridge: ${process.env.MQTT_URL} (topic "${process.env.MQTT_TOPIC || 'bioreactor'}")`);
  }
});
