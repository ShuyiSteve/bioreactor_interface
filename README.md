# Connected Bioreactor Control System

A web dashboard, REST API and SQLite datastore for the small-scale bioreactor
testbed described in *"Challenge 2: Connected Bioreactor Design Specification"*.
It monitors and controls the three subsystems — **temperature**, **pH** and
**stirring speed** — and logs every sample to SQLite.

Following the *Connectivity* specification, two ESP32-family boards are used:
a **Nano ESP32** runs the local control loop next to the hardware, and an
**ESP32 DevKitC** bridges it to the network. They are linked over **UART**, and
the bridge talks to the server over **WiFi using MQTT** (publish/subscribe). An
HTTP ingest path is also provided as a fallback.

```
  Nano ESP32 (controller)        DevKitC (bridge)          MQTT broker
 +--------------------+  UART   +-----------------+   publish / subscribe
 | temp / pH / RPM    | ------> | WiFi + MQTT     | <--------------------+
 | heater/pumps/motor | <------ | JSON lines      |                      |
 +--------------------+         +-----------------+                      |
                                                                         v
                                                              +-------------------+
                                                              |  Node.js server   |
                                                              |  Express + SQLite |
                                                              +---------+---------+
                                                                        |
                                                               +--------v--------+
                                                               |    Dashboard    |
                                                               |     (React)     |
                                                               +-----------------+
```

## What's in this repo

| Path | Purpose |
| --- | --- |
| `backend/server.js` | Express API + built-in live simulator (demo mode) |
| `backend/db.js` | SQLite schema and data-access layer (`node:sqlite`) |
| `backend/seed.js` | Seeds realistic demo history; `--reset` wipes the DB |
| `frontend/` | React + TypeScript + Vite dashboard |
| `backend/mqtt.js` | Optional MQTT bridge (telemetry in, commands out) |
| `firmware/nano_controller/` | Nano ESP32: sensors, actuators, control loop, UART |
| `firmware/devkit_bridge/` | DevKitC: UART ↔ WiFi/MQTT bridge |
| `firmware/esp32_bioreactor/` | Single-board HTTP alternative (no MQTT) |

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
| `MQTT_URL` | *(unset)* | MQTT broker, e.g. `mqtt://broker.hivemq.com:1883` |
| `MQTT_TOPIC` | `bioreactor` | MQTT topic prefix |
| `MQTT_DEVICE_ID` | `bio1` | Device id used for outgoing MQTT commands |

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

### Data flow

Two equivalent paths are supported:

**MQTT (recommended, per the Connectivity spec).** The DevKitC bridge publishes
each sample to `bioreactor/<device>/telemetry` and subscribes to
`bioreactor/<device>/commands`. The server mirrors this: it subscribes to
`bioreactor/+/telemetry`, writes the JSON into SQLite, and publishes operator
commands to `bioreactor/<device>/commands`.

**HTTP (fallback / single-board).** A board can `POST` a reading to `/api/ingest`
and either read the returned commands or poll `/api/commands/pending`. The
payload is the same JSON:
`{"temperature":29.5,"ph":5.1,"rpm":1002,"heater":1,"pump_acid":0,"pump_alkali":0,"stirrer":1}`

## SQLite schema

- `readings` — one row per sample (`ts`, `temperature`, `ph`, `rpm`, actuator flags)
- `settings` — single row of operator setpoints + mode (`auto`/`manual`)
- `control_state` — single row of actuator states
- `commands` — outbound command queue for two-way communication
- `events` — human-readable activity log

## Firmware and wiring (two boards)

Install in the Arduino IDE: the **esp32 by Espressif** board package, **PicoMQTT**
and **ArduinoJson**.

### 1. `firmware/nano_controller/nano_controller.ino` (Arduino Nano ESP32)

Runs the control loop and talks to the hardware. Edit the pin map and pH
calibration constants at the top.

| Signal | Pin | Notes |
| --- | --- | --- |
| Thermistor divider | `A0` | 10 kΩ NTC + 10 kΩ series resistor |
| pH probe board | `A1` | analog conditioning output |
| IR reflective sensor | `2` | 2 pulses / revolution |
| Heater relay | `3` | 3 Ω, 30 W element |
| Acid pump | `4` | 6 V peristaltic pump |
| Alkali pump | `5` | 6 V peristaltic pump |
| Stirrer PWM | `6` | motor driver |

### 2. `firmware/devkit_bridge/devkit_bridge.ino` (ESP32 DevKitC V4)

Bridges UART to WiFi/MQTT. Edit `WIFI_SSID`, `WIFI_PASS`, `MQTT_BROKER`,
`DEVICE_ID` and `TOPIC`. Public brokers from the spec: `broker.hivemq.com`,
`test.mosquitto.org`, or `engf0001.cs.ucl.ac.uk` (UCL network only).

**UART link between the boards** (crossed, 115200 baud, line-delimited JSON):

| Nano ESP32 | DevKitC |
| --- | --- |
| TX `D1` | RX2 `GPIO16` |
| RX `D0` | TX2 `GPIO17` |
| GND | GND |

### Single-board HTTP alternative

`firmware/esp32_bioreactor/esp32_bioreactor.ino` does everything on one ESP32 and
talks straight to the server over HTTP. Set `SERVER` to your computer's LAN IP
(`http://192.168.1.50:3001`, **not** `localhost`) and ignore MQTT.

## Design notes

- **Auto mode**: the on-device (or simulated) hysteresis controller holds each
  parameter at its setpoint. The dashboard shows the live deviation against the
  tolerance band (green = in spec, amber = out of spec).
- **Manual mode**: operator toggles the heater, acid/alkali pumps and stirrer
  directly; commands are queued and delivered to the device.
- Charts shade the tolerance band around the setpoint line so it is easy to see
  whether the process is within specification.
