# Connected Bioreactor Control System

A web dashboard, REST API and SQLite datastore for the small-scale bioreactor
testbed described in *"Challenge 2: Connected Bioreactor Design Specification"*.
It monitors and controls the three subsystems — **temperature**, **pH** and
**stirring speed** — and logs every sample to SQLite.

The firmware talks to the server over WiFi via an **ESP32** (programmed with the
Arduino IDE). The dashboard sends setpoints and actuator commands back down the
same channel, so the loop is fully two-way.

```
                     +----------------+   HTTP /api/ingest   +-----------------+
 [thermistor] -----> |                | -------------------> |                 |
 [pH probe] -------> |      ESP32     |                      |  Node.js server |
 [IR sensor] ------> |   (Arduino)    | <------------------- |  Express + API  |
                     |                |   commands (JSON)    |                 |
 [heater/pumps] <--- |                |                      |  SQLite (WAL)   |
 [stirrer motor] <-- |                |                      +--------+--------+
                     +----------------+                               |
                                                               +------v------+
                                                               |  Dashboard  |
                                                               |  (React)    |
                                                               +-------------+
```

## What's in this repo

| Path | Purpose |
| --- | --- |
| `backend/server.js` | Express API + built-in live simulator (demo mode) |
| `backend/db.js` | SQLite schema and data-access layer (`node:sqlite`) |
| `backend/seed.js` | Seeds realistic demo history; `--reset` wipes the DB |
| `frontend/` | React + TypeScript + Vite dashboard |
| `firmware/esp32_bioreactor/` | ESP32/Arduino sketch (sensors, control loop, WiFi) |

## Control ranges (from the specification)

| Parameter | Setpoint range | Tolerance |
| --- | --- | --- |
| Temperature | 25–35 °C | ± 0.5 °C |
| Stirring speed | 500–1500 RPM | ± 20 RPM |
| pH | 3–7 (default 5) | sensing range |

## Quick start

Requires **Node.js ≥ 22.5** (the backend uses the built-in `node:sqlite` module).

```bash
# 1. install backend dependencies
npm install

# 2. install frontend dependencies
npm --prefix frontend install

# 3. run API + dev dashboard together (API :3001, UI :5173)
npm run dev
```

Open <http://localhost:5173>. The UI polls the API every few seconds.

The backend starts in **simulation mode** by default, so the dashboard is live
even without hardware: it generates readings and a hysteresis control loop every
2.5 seconds. Point a real ESP32 at the server to replace the simulated data.

### Production-style single server

```bash
npm run build    # builds frontend/dist
npm start        # serves API + dashboard on http://localhost:3001
```

## Configuration

Backend environment variables:

| Variable | Default | Description |
| --- | --- | --- |
| `PORT` | `3001` | HTTP port |
| `DB_PATH` | `backend/bioreactor.db` | SQLite file location |
| `SIMULATE` | `true` | Set `false` to disable the live simulator |
| `SEED` | `true` | Seed demo history when the DB is empty |

```bash
PORT=3001 SIMULATE=false npm start
```

Reset the database:

```bash
npm run reset-db    # deletes the .db file; recreated + re-seeded on next start
```

## API reference

| Method | Endpoint | Description |
| --- | --- | --- |
| `GET` | `/api/status` | Latest reading, setpoints, actuator states, device online status |
| `GET` | `/api/readings?param=temp\|ph\|rpm&range=10m&points=140` | Time series for charting |
| `GET` | `/api/log?limit=100` | Most recent readings |
| `GET` | `/api/events?limit=40` | Activity feed |
| `POST` | `/api/setpoints` | `{ temp?, ph?, rpm?, mode? }` — clamped to spec |
| `POST` | `/api/control` | `{ heater?, pump_acid?, pump_alkali?, stirrer? }` |
| `POST` | `/api/ingest` | ESP32 pushes a reading; response includes pending commands |
| `GET` | `/api/commands/pending` | ESP32 polls pending commands |
| `POST` | `/api/commands/ack` | `{ ids: [...] }` acknowledge commands |
| `GET` | `/api/health` | Liveness check |

### ESP32 data flow

1. Every 2 s the ESP32 `POST`s a reading to `/api/ingest`:
   `{"temperature":29.5,"ph":5.1,"rpm":1002,"heater":1,"pump_acid":0,"pump_alkali":0,"stirrer":1}`
2. The server stores it in SQLite and replies with any queued commands.
3. The ESP32 also polls `/api/commands/pending` every 3 s and applies
   `setpoints` / `control` commands (then the dashboard updates its UI from
   `/api/status`).

## SQLite schema

- `readings` — one row per sample (`ts`, `temperature`, `ph`, `rpm`, actuator flags)
- `settings` — single row of operator setpoints + mode (`auto`/`manual`)
- `control_state` — single row of actuator states
- `commands` — outbound command queue for two-way communication
- `events` — human-readable activity log

## ESP32 wiring and firmware

Open `firmware/esp32_bioreactor/esp32_bioreactor.ino` in the Arduino IDE, install
the **ESP32** board package and the **ArduinoJson** library, then edit the config
block at the top:

- `WIFI_SSID` / `WIFI_PASS`
- `SERVER` — your computer's LAN IP, e.g. `http://192.168.1.50:3001` (**not** `localhost`)
- pin map and pH calibration constants

Suggested pin map (change to match your board):

| Signal | Pin | Notes |
| --- | --- | --- |
| Thermistor divider | `34` | 10 kΩ NTC + 10 kΩ series resistor |
| pH probe board | `35` | analog conditioning output |
| IR reflective sensor | `32` | 2 pulses / revolution |
| Heater relay | `26` | 3 Ω, 30 W element |
| Acid pump | `27` | 6 V peristaltic pump |
| Alkali pump | `14` | 6 V peristaltic pump |
| Stirrer PWM | `25` | motor driver |

> The specification describes an **Arduino Uno bridged to the ESP32**. If you keep
> that two-board layout, put the sensor/actuator code on the Uno and send compact
> lines over UART to the ESP32, which forwards them to the server (see the comment
> at the top of the sketch).

## Design notes

- **Auto mode**: the on-device (or simulated) hysteresis controller holds each
  parameter at its setpoint. The dashboard shows the live deviation against the
  tolerance band (green = in spec, amber = out of spec).
- **Manual mode**: operator toggles the heater, acid/alkali pumps and stirrer
  directly; commands are queued and delivered to the device.
- Charts shade the tolerance band around the setpoint line so it is easy to see
  whether the process is within specification.
