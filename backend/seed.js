// Seeds the database with realistic demo history so the dashboard has data on
// first launch. Run `node backend/seed.js --reset` to wipe the database (it is
// recreated and re-seeded automatically on the next server start).
'use strict';

const fs = require('node:fs');
const db = require('./db');

function generateDemoData({ minutes = 45, intervalMs = 4000 } = {}) {
  const end = Date.now();
  const start = end - minutes * 60 * 1000;
  const duration = end - start;

  let temp = 19.2;
  let ph = 6.35;
  let rpm = 180;
  let heater = 0;
  let acid = 0;
  let alkali = 0;

  const TARGET_TEMP = 30.0;
  const TARGET_PH = 5.0;
  const TARGET_RPM = 1000;

  const insert = db.insertReading;

  for (let ts = start; ts <= end; ts += intervalMs) {
    const progress = (ts - start) / duration; // 0..1

    // Temperature: first-order rise toward setpoint, then oscillate around it.
    temp += (TARGET_TEMP - temp) * 0.02 + (Math.random() - 0.5) * 0.12;
    temp += Math.sin(progress * Math.PI * 14) * 0.02;

    // pH: drift down toward the default optimum with light noise.
    ph += (TARGET_PH - ph) * 0.012 + (Math.random() - 0.5) * 0.03;

    // RPM: ramp up then hold with jitter.
    rpm += (TARGET_RPM - rpm) * 0.04 + (Math.random() - 0.5) * 8;

    // Simple on/off hysteresis so the states look realistic.
    if (temp < TARGET_TEMP - 0.25) heater = 1;
    else if (temp > TARGET_TEMP + 0.25) heater = 0;

    acid = ph > TARGET_PH + 0.06 ? 1 : 0;
    alkali = ph < TARGET_PH - 0.06 ? 1 : 0;

    insert({
      ts,
      temperature: Math.round(temp * 100) / 100,
      ph: Math.round(Math.min(7.5, Math.max(2.5, ph)) * 100) / 100,
      rpm: Math.max(0, Math.round(rpm)),
      heater,
      pump_acid: acid,
      pump_alkali: alkali,
      stirrer: 1,
    });
  }

  db.logEvent('info', 'Demo history seeded into SQLite database.');
}

function main() {
  if (process.argv.includes('--reset')) {
    db.db.close();
    fs.rmSync(db.DB_PATH, { force: true });
    console.log(`Deleted ${db.DB_PATH}. It will be recreated and re-seeded on next start.`);
    return;
  }

  if (db.countReadings() === 0) {
    generateDemoData();
    console.log(`Seeded demo data into ${db.DB_PATH}`);
  } else {
    console.log(`Database already has ${db.countReadings()} readings; skipping seed.`);
  }
}

if (require.main === module) {
  main();
}

module.exports = { generateDemoData };
