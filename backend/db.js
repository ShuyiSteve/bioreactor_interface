// SQLite persistence layer for the bioreactor control system.
// Uses Node's built-in `node:sqlite` (Node >= 22.5) so there are no native
// dependencies to compile or download.
'use strict';

process.env.NODE_NO_WARNINGS = process.env.NODE_NO_WARNINGS || '1';

const { DatabaseSync } = require('node:sqlite');
const path = require('node:path');
const fs = require('node:fs');

const DB_PATH = process.env.DB_PATH || path.join(__dirname, 'bioreactor.db');
fs.mkdirSync(path.dirname(DB_PATH), { recursive: true });

const db = new DatabaseSync(DB_PATH);

db.exec(`
  PRAGMA journal_mode = WAL;
  PRAGMA synchronous = NORMAL;

  -- One row per sensor sample pushed by the ESP32 (or the built-in simulator).
  CREATE TABLE IF NOT EXISTS readings (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    ts          INTEGER NOT NULL,
    seq         INTEGER,
    device      TEXT,
    temperature REAL,
    ph          REAL,
    rpm         REAL,
    heater      INTEGER NOT NULL DEFAULT 0,
    pump_acid   INTEGER NOT NULL DEFAULT 0,
    pump_alkali INTEGER NOT NULL DEFAULT 0,
    stirrer     INTEGER NOT NULL DEFAULT 0,
    raw         TEXT
  );

  CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings (ts);

  -- Operator-adjustable setpoints (single row, id = 1).
  CREATE TABLE IF NOT EXISTS settings (
    id            INTEGER PRIMARY KEY CHECK (id = 1),
    temp_setpoint REAL NOT NULL DEFAULT 30.0,
    ph_setpoint   REAL NOT NULL DEFAULT 5.0,
    rpm_setpoint  REAL NOT NULL DEFAULT 1000,
    mode          TEXT NOT NULL DEFAULT 'auto',
    updated_at    INTEGER NOT NULL DEFAULT 0
  );

  -- Actuator states (single row, id = 1). In "auto" mode the device decides
  -- these; in "manual" mode they are operator overrides.
  CREATE TABLE IF NOT EXISTS control_state (
    id          INTEGER PRIMARY KEY CHECK (id = 1),
    heater      INTEGER NOT NULL DEFAULT 0,
    pump_acid   INTEGER NOT NULL DEFAULT 0,
    pump_alkali INTEGER NOT NULL DEFAULT 0,
    stirrer     INTEGER NOT NULL DEFAULT 1,
    updated_at  INTEGER NOT NULL DEFAULT 0
  );

  -- Outbound command queue for two-way communication with the ESP32.
  CREATE TABLE IF NOT EXISTS commands (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    command_id   TEXT,
    type         TEXT NOT NULL,
    payload      TEXT NOT NULL,
    status       TEXT NOT NULL DEFAULT 'pending',
    created_at   INTEGER NOT NULL,
    delivered_at INTEGER,
    applied      INTEGER
  );

  -- Human-readable activity log.
  CREATE TABLE IF NOT EXISTS events (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    ts      INTEGER NOT NULL,
    level   TEXT NOT NULL DEFAULT 'info',
    message TEXT NOT NULL
  );
`);

const now = Date.now();
db.prepare(
  `INSERT OR IGNORE INTO settings (id, temp_setpoint, ph_setpoint, rpm_setpoint, mode, updated_at)
   VALUES (1, 30.0, 5.0, 1000, 'auto', ?)`
).run(now);
db.prepare(
  `INSERT OR IGNORE INTO control_state (id, heater, pump_acid, pump_alkali, stirrer, updated_at)
   VALUES (1, 0, 0, 0, 1, ?)`
).run(now);

// Lightweight migration: add columns introduced after the DB was first created.
function ensureColumn(table, column, ddl) {
  const cols = db.prepare(`PRAGMA table_info(${table})`).all();
  if (!cols.some((c) => c.name === column)) {
    db.exec(`ALTER TABLE ${table} ADD COLUMN ${ddl}`);
  }
}
ensureColumn('readings', 'seq', 'seq INTEGER');
ensureColumn('readings', 'device', 'device TEXT');
ensureColumn('readings', 'raw', 'raw TEXT');
ensureColumn('commands', 'command_id', 'command_id TEXT');
ensureColumn('commands', 'applied', 'applied INTEGER');

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function num(v, fallback = null) {
  const n = Number(v);
  return Number.isFinite(n) ? n : fallback;
}

function bool(v, fallback = 0) {
  if (v === null || v === undefined) return fallback ? 1 : 0;
  return v ? 1 : 0;
}

function clamp(v, min, max, fallback) {
  const n = num(v, NaN);
  if (Number.isNaN(n)) return fallback;
  return Math.min(max, Math.max(min, n));
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

function getSettings() {
  const row = db
    .prepare(
      'SELECT temp_setpoint AS temp, ph_setpoint AS ph, rpm_setpoint AS rpm, mode FROM settings WHERE id = 1'
    )
    .get();
  return { temp: row.temp, ph: row.ph, rpm: row.rpm, mode: row.mode };
}

function updateSettings({ temp, ph, rpm, mode }) {
  db.prepare(
    'UPDATE settings SET temp_setpoint = ?, ph_setpoint = ?, rpm_setpoint = ?, mode = ?, updated_at = ? WHERE id = 1'
  ).run(temp, ph, rpm, mode, Date.now());
  return getSettings();
}

// ---------------------------------------------------------------------------
// Control state
// ---------------------------------------------------------------------------

function getControlState() {
  const row = db
    .prepare(
      'SELECT heater, pump_acid, pump_alkali, stirrer FROM control_state WHERE id = 1'
    )
    .get();
  return {
    heater: row.heater,
    pump_acid: row.pump_acid,
    pump_alkali: row.pump_alkali,
    stirrer: row.stirrer,
  };
}

function updateControlState({ heater, pump_acid, pump_alkali, stirrer }) {
  db.prepare(
    'UPDATE control_state SET heater = ?, pump_acid = ?, pump_alkali = ?, stirrer = ?, updated_at = ? WHERE id = 1'
  ).run(
    bool(heater),
    bool(pump_acid),
    bool(pump_alkali),
    bool(stirrer),
    Date.now()
  );
  return getControlState();
}

// ---------------------------------------------------------------------------
// Readings
// ---------------------------------------------------------------------------

function insertReading({
  temperature,
  ph,
  rpm,
  heater = 0,
  pump_acid = 0,
  pump_alkali = 0,
  stirrer = 0,
  ts = Date.now(),
  seq = null,
  device = null,
  raw = null,
}) {
  const info = db
    .prepare(
      `INSERT INTO readings (ts, seq, device, temperature, ph, rpm, heater, pump_acid, pump_alkali, stirrer, raw)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`
    )
    .run(
      ts,
      seq === null ? null : Number(seq),
      device === null ? null : String(device),
      num(temperature),
      num(ph),
      num(rpm),
      bool(heater),
      bool(pump_acid),
      bool(pump_alkali),
      bool(stirrer),
      raw === null ? null : (typeof raw === 'string' ? raw : JSON.stringify(raw))
    );
  return Number(info.lastInsertRowid);
}

function getLatest() {
  const row = db
    .prepare(
      'SELECT ts, temperature, ph, rpm, heater, pump_acid, pump_alkali, stirrer, raw FROM readings ORDER BY ts DESC, id DESC LIMIT 1'
    )
    .get();
  return row ? { ...row, raw: row.raw ? safeParse(row.raw) : null } : null;
}

function countReadings() {
  return db.prepare('SELECT COUNT(*) AS n FROM readings').get().n;
}

const READING_COLUMNS = { temp: 'temperature', ph: 'ph', rpm: 'rpm' };

function getSeries(param, rangeMs, points) {
  const col = READING_COLUMNS[param];
  if (!col) throw new Error(`Unknown parameter: ${param}`);
  const since = Date.now() - rangeMs;
  const rows = db
    .prepare(`SELECT ts, ${col} AS v FROM readings WHERE ts >= ? ORDER BY ts ASC`)
    .all(since);
  return downsample(rows, Math.max(2, Number(points) || 120));
}

function getLog(limit) {
  const n = Math.min(1000, Math.max(1, Number(limit) || 100));
  const rows = db
    .prepare(
      'SELECT ts, seq, device, temperature, ph, rpm, heater, pump_acid, pump_alkali, stirrer, raw FROM readings ORDER BY ts DESC, id DESC LIMIT ?'
    )
    .all(n);
  return rows.map((r) => ({ ...r, raw: r.raw ? safeParse(r.raw) : null }));
}

function downsample(rows, points) {
  if (rows.length <= points) return rows;
  const step = (rows.length - 1) / (points - 1);
  const out = [];
  for (let i = 0; i < points; i++) {
    out.push(rows[Math.round(i * step)]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Commands (two-way communication queue)
// ---------------------------------------------------------------------------

const { randomUUID } = require('node:crypto');

function enqueueCommand(type, payload) {
  const commandId = randomUUID();
  db.prepare(
    'INSERT INTO commands (command_id, type, payload, status, created_at) VALUES (?, ?, ?, ?, ?)'
  ).run(commandId, type, JSON.stringify(payload), 'pending', Date.now());
  return { id: commandId, type, payload };
}

function getPendingCommands(markSent = false) {
  const rows = db
    .prepare(
      "SELECT command_id AS cid, type, payload FROM commands WHERE status = 'pending' ORDER BY id ASC"
    )
    .all();
  if (markSent && rows.length) {
    const ids = rows.map((r) => r.cid);
    const placeholders = ids.map(() => '?').join(',');
    db.prepare(
      `UPDATE commands SET status = 'sent', delivered_at = ? WHERE command_id IN (${placeholders})`
    ).run(Date.now(), ...ids);
  }
  return rows.map((r) => ({
    id: r.cid,
    type: r.type,
    payload: safeParse(r.payload),
  }));
}

function ackCommand(commandId, ok) {
  if (!commandId) return 0;
  const info = db
    .prepare(`UPDATE commands SET status = 'acked', applied = ? WHERE command_id = ?`)
    .run(ok ? 1 : 0, String(commandId));
  return Number(info.changes);
}

// Most recent set-point command, for the dashboard's "confirmed" indicator.
function getLastCommand(type) {
  return (
    db
      .prepare(
        'SELECT command_id AS id, status, created_at AS createdAt, applied FROM commands WHERE type = ? ORDER BY id DESC LIMIT 1'
      )
      .get(type) || null
  );
}

function ackCommands(ids) {
  if (!Array.isArray(ids) || ids.length === 0) return 0;
  let changed = 0;
  for (const id of ids) {
    if (id !== null && id !== undefined) changed += ackCommand(String(id), true);
  }
  return changed;
}

function safeParse(text) {
  try {
    return JSON.parse(text);
  } catch {
    return {};
  }
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

function logEvent(level, message) {
  db.prepare('INSERT INTO events (ts, level, message) VALUES (?, ?, ?)').run(
    Date.now(),
    level,
    message
  );
}

function getEvents(limit) {
  const n = Math.min(200, Math.max(1, Number(limit) || 50));
  return db
    .prepare('SELECT ts, level, message FROM events ORDER BY ts DESC, id DESC LIMIT ?')
    .all(n);
}

module.exports = {
  db,
  DB_PATH,
  getSettings,
  updateSettings,
  getControlState,
  updateControlState,
  insertReading,
  getLatest,
  countReadings,
  getSeries,
  getLog,
  enqueueCommand,
  getPendingCommands,
  ackCommand,
  ackCommands,
  getLastCommand,
  logEvent,
  getEvents,
  num,
  bool,
  clamp,
};
