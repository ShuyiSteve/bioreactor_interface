/*
 * Bioreactor — Arduino Nano ESP32 (ESP32-S3) controller
 * ======================================================
 * Sits next to the hardware: reads sensors, drives actuators and runs the local
 * control loop. It does NOT connect to WiFi — it exchanges line-delimited JSON
 * with the DevKitC bridge over UART.
 *
 * Implemented from the team spec (items 1-6):
 *   1. Heater is PWM (ledcWrite) with a hard 60% duty cap (never 100% = 48 W
 *      on a 30 W element).
 *   2. Telemetry uses the course JSON shape, sent every 1 s:
 *        { device, seq, window{start,end,seconds,samples},
 *          temperature_C/pH/rpm -> {mean,min,max},   // ~10 readings averaged
 *          actuators_avg -> {heater_pwm, motor_pwm, acid_pwm, base_pwm},  // 0..1
 *          setpoints -> {temperature_C, pH, rpm}, mode, sp_source, alarms[] }
 *      The DevKitC adds the unix timestamps (NTP).
 *   3. Commands carry an id, use temperature_C (not temp), and the Nano
 *      range-checks every setpoint (25-35 C, 500-1500 RPM, pH 3-7) and replies
 *      on the UART with {id, ok, applied} (the bridge forwards it to MQTT ack).
 *   5. Stirring uses a Hall sensor at 70 pulses/rev and PI control on the RPM
 *      error (no longer a straight setpoint -> PWM map).
 *
 * Wiring to the DevKitC (UART, crossed):
 *   Nano TX (D1)  -> DevKit RX2 (GPIO16)
 *   Nano RX (D0)  <- DevKit TX2 (GPIO17)
 *   GND           <-> GND
 *
 * Required libraries: ArduinoJson (Benoit Blanchon)
 * Board package: Arduino Nano ESP32
 */

#include <ArduinoJson.h>

// ---------------------------------------------------------------- identity
const char* DEVICE_ID = "bio1";   // matches MQTT_DEVICE_ID on the server

// ---------------------------------------------------------------- pin map
const int PIN_TEMP_ADC    = A0;  // thermistor divider (10 k NTC + 10 k series)
const int PIN_PH_ADC      = A1;  // pH probe conditioning board
const int PIN_RPM_SENSOR  = 2;   // RPM sensor (Hall effect = 70 pulses/rev)
const int PIN_HEATER      = 3;   // heater (PWM, 3 Ohm / 30 W)
const int PIN_PUMP_ACID   = 4;   // acid peristaltic pump
const int PIN_PUMP_ALKALI = 5;   // alkali (base) peristaltic pump
const int PIN_STIRRER_PWM = 6;   // stirring motor (PWM)

// UART + timing.
const unsigned long UART_BAUD  = 115200;
const unsigned long SAMPLE_MS  = 100;    // sample sensors at 10 Hz
const unsigned long WINDOW_MS  = 1000;   // 1 s telemetry window

// Thermistor (10 kOhm NTC, beta model).
const float R_REF  = 10000.0f;
const float R_NOM  = 10000.0f;
const float B_COEF = 3950.0f;
const float T_NOM  = 298.15f;

// pH calibration — CALIBRATE WITH BUFFER SOLUTIONS.
const float PH_SLOPE  = -5.70f;
const float PH_OFFSET = 21.34f;

// RPM sensor.
const float PULSES_PER_REV = 70.0f;  // Hall sensor: 70 pulses/rev (item 5)

// Heater safety cap.
const float HEATER_MAX = 0.60f;      // 60% max duty

// Stirring PI controller (item 5): tracks RPM instead of open-loop mapping.
const float KP = 0.0005f;            // duty per RPM of error
const float KI = 0.0003f;            // duty per (RPM·s) of accumulated error
const float RPM_INTEGRAL_LIMIT = 0.5f / KI;
float rpmIntegral = 0.0f;

// Control setpoints (overwritten by commands from the dashboard).
float setpointTemp = 30.0f;
float setpointPh   = 5.0f;
float setpointRpm  = 1000.0f;
String mode        = "auto";
String spSource    = "default";      // "default" | "dashboard"

// Actuator duties (0..1). Pumps are on/off so their duty is 0 or 1.
float heaterDuty = 0.0f;
float motorDuty  = 0.0f;
float acidDuty   = 0.0f;
float baseDuty   = 0.0f;

// RPM measurement.
volatile unsigned long pulseCount = 0;
unsigned long lastRpmMillis = 0;
float measuredRpm = 0.0f;

// Telemetry window accumulators.
unsigned long windowStart = 0;
int sampleCount = 0;
float tempSum = 0, tempMin = 1e9f, tempMax = -1e9f;
float phSum = 0,   phMin = 1e9f,   phMax = -1e9f;
float rpmSum = 0,  rpmMin = 1e9f,  rpmMax = -1e9f;
unsigned long seq = 0;

unsigned long lastSample = 0;
String rxLine = "";

void IRAM_ATTR rpmISR() { pulseCount++; }

// ---------------------------------------------------------------- sensors
float readTemperatureC() {
  int raw = analogRead(PIN_TEMP_ADC);
  if (raw <= 1) raw = 1;
  float r = R_REF * ((4095.0f / raw) - 1.0f);
  float tempK = 1.0f / ((1.0f / T_NOM) + logf(r / R_NOM) / B_COEF);
  return tempK - 273.15f;
}

float readPH() {
  int raw = analogRead(PIN_PH_ADC);
  float volts = raw * (3.3f / 4095.0f);
  return PH_SLOPE * volts + PH_OFFSET;
}

float readRPM() {
  unsigned long now = millis();
  float elapsed = (now - lastRpmMillis) / 1000.0f;
  if (elapsed < 0.05f) return measuredRpm;
  measuredRpm = (pulseCount * 60.0f) / PULSES_PER_REV / elapsed;
  pulseCount = 0;
  lastRpmMillis = now;
  return measuredRpm;
}

// ---------------------------------------------------------------- actuators
// Arduino-ESP32 core 2.x (e.g. the Arduino Nano ESP32 board package) uses
// ledcSetup + ledcAttachPin with a channel number, while core 3.x uses
// ledcAttach with the pin directly. These wrappers keep the control code
// identical on both cores.
static const int LEDC_CH_HEATER  = 0;
static const int LEDC_CH_STIRRER = 1;
static const int LEDC_FREQ       = 5000;   // 5 kHz
static const int LEDC_RES        = 8;      // 8-bit duty

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void pwmAttach(int pin, int channel, int freq, int res) {
  (void)channel;
  ledcAttach(pin, freq, res);
}
void pwmWrite(int pin, int channel, int duty) {
  (void)channel;
  ledcWrite(pin, duty);
}
#else
void pwmAttach(int pin, int channel, int freq, int res) {
  ledcSetup(channel, freq, res);
  ledcAttachPin(pin, channel);
}
void pwmWrite(int pin, int channel, int duty) {
  (void)pin;
  ledcWrite(channel, duty);
}
#endif

void setHeaterDuty(float d) {
  heaterDuty = constrain(d, 0.0f, HEATER_MAX);   // hard 60% cap
  pwmWrite(PIN_HEATER, LEDC_CH_HEATER, (int)(heaterDuty * 255.0f));
}

void setMotorDuty(float d) {
  motorDuty = constrain(d, 0.0f, 1.0f);
  pwmWrite(PIN_STIRRER_PWM, LEDC_CH_STIRRER, (int)(motorDuty * 255.0f));
}

void setAcidDuty(float d) {
  acidDuty = d > 0.5f ? 1.0f : 0.0f;
  digitalWrite(PIN_PUMP_ACID, acidDuty > 0.5f ? HIGH : LOW);
}

void setBaseDuty(float d) {
  baseDuty = d > 0.5f ? 1.0f : 0.0f;
  digitalWrite(PIN_PUMP_ALKALI, baseDuty > 0.5f ? HIGH : LOW);
}

void updateStirrerPI(float rpm) {
  if (setpointRpm <= 0) {
    setMotorDuty(0.0f);
    rpmIntegral = 0.0f;
    return;
  }
  float error = setpointRpm - rpm;
  rpmIntegral += error * (SAMPLE_MS / 1000.0f);              // dt = sample period
  rpmIntegral = constrain(rpmIntegral, -RPM_INTEGRAL_LIMIT, RPM_INTEGRAL_LIMIT);  // anti-windup
  setMotorDuty(KP * error + KI * rpmIntegral);
}

void controlLoop(float t, float p, float r) {
  if (mode != "auto") return;   // manual mode: duties come from control commands

  // Heater: hysteresis at the 60% cap (never 100%).
  if (t < setpointTemp - 0.25f) setHeaterDuty(HEATER_MAX);
  else if (t > setpointTemp + 0.25f) setHeaterDuty(0.0f);

  // pH: add acid / base.
  if (p > setpointPh + 0.1f) { setAcidDuty(1.0f); setBaseDuty(0.0f); }
  else if (p < setpointPh - 0.1f) { setBaseDuty(1.0f); setAcidDuty(0.0f); }
  else { setAcidDuty(0.0f); setBaseDuty(0.0f); }

  // Stirring: PI control on RPM error (item 5).
  updateStirrerPI(r);
}

// ---------------------------------------------------------------- telemetry
void resetWindow(unsigned long startMs) {
  windowStart = startMs;
  sampleCount = 0;
  tempSum = phSum = rpmSum = 0.0f;
  tempMin = phMin = rpmMin = 1e9f;
  tempMax = phMax = rpmMax = -1e9f;
}

void accumulate(float t, float p, float r) {
  tempSum += t; tempMin = min(tempMin, t); tempMax = max(tempMax, t);
  phSum   += p; phMin   = min(phMin,   p); phMax   = max(phMax,   p);
  rpmSum  += r; rpmMin  = min(rpmMin,  r); rpmMax  = max(rpmMax,  r);
  sampleCount++;
}

void sendTelemetry(unsigned long endMs) {
  float n = sampleCount > 0 ? (float)sampleCount : 1.0f;

  JsonDocument doc;
  doc["device"] = DEVICE_ID;
  doc["seq"] = ++seq;

  JsonObject win = doc.createNestedObject("window");
  win["start"] = windowStart;                       // ms since Nano boot (DevKitC stamps unix)
  win["end"] = endMs;
  win["seconds"] = (endMs - windowStart) / 1000.0f;
  win["samples"] = sampleCount;

  JsonObject tc = doc.createNestedObject("temperature_C");
  tc["mean"] = tempSum / n; tc["min"] = tempMin; tc["max"] = tempMax;
  JsonObject pc = doc.createNestedObject("pH");
  pc["mean"] = phSum / n; pc["min"] = phMin; pc["max"] = phMax;
  JsonObject rc = doc.createNestedObject("rpm");
  rc["mean"] = rpmSum / n; rc["min"] = rpmMin; rc["max"] = rpmMax;

  JsonObject act = doc.createNestedObject("actuators_avg");
  act["heater_pwm"] = heaterDuty;
  act["motor_pwm"]  = motorDuty;
  act["acid_pwm"]   = acidDuty;
  act["base_pwm"]   = baseDuty;

  JsonObject sp = doc.createNestedObject("setpoints");
  sp["temperature_C"] = setpointTemp;
  sp["pH"] = setpointPh;
  sp["rpm"] = setpointRpm;

  doc["mode"] = mode;
  doc["sp_source"] = spSource;
  doc.createNestedArray("alarms");   // empty for now

  serializeJson(doc, Serial1);
  Serial1.print('\n');
}

// ---------------------------------------------------------------- commands
void sendSetpointAck(const char* id, bool ok) {
  JsonDocument doc;
  doc["id"] = id;
  doc["ok"] = ok;
  JsonObject ap = doc.createNestedObject("applied");
  ap["temperature_C"] = setpointTemp;
  ap["pH"] = setpointPh;
  ap["rpm"] = setpointRpm;
  serializeJson(doc, Serial1);
  Serial1.print('\n');
}

void sendControlAck(const char* id, bool ok) {
  JsonDocument doc;
  doc["id"] = id;
  doc["ok"] = ok;
  JsonObject ap = doc.createNestedObject("applied");
  ap["heater_pwm"] = heaterDuty;
  ap["motor_pwm"]  = motorDuty;
  ap["acid_pwm"]   = acidDuty;
  ap["base_pwm"]   = baseDuty;
  serializeJson(doc, Serial1);
  Serial1.print('\n');
}

void applyCommand(const JsonDocument& cmd) {
  const char* id = cmd["id"] | "";
  const char* type = cmd["type"] | "";
  JsonObjectConst p = cmd["payload"].as<JsonObjectConst>();

  if (String(type) == "setpoints") {
    bool ok = true;

    if (p["temperature_C"].is<float>()) {
      float v = p["temperature_C"].as<float>();
      ok = ok && (v >= 25.0f && v <= 35.0f);
      setpointTemp = constrain(v, 25.0f, 35.0f);
    }
    if (p["pH"].is<float>()) {
      float v = p["pH"].as<float>();
      ok = ok && (v >= 3.0f && v <= 7.0f);
      setpointPh = constrain(v, 3.0f, 7.0f);
    }
    if (p["rpm"].is<float>()) {
      float v = p["rpm"].as<float>();
      ok = ok && (v >= 500.0f && v <= 1500.0f);
      setpointRpm = constrain(v, 500.0f, 1500.0f);
    }
    if (p["mode"].is<const char*>()) mode = p["mode"].as<const char*>();

    spSource = "dashboard";
    sendSetpointAck(id, ok);
    Serial.printf("[cmd] setpoints %.1f C / pH %.1f / %.0f RPM / %s (ok=%d)\n",
                  setpointTemp, setpointPh, setpointRpm, mode.c_str(), ok);
  } else if (String(type) == "control") {
    if (p["heater_pwm"].is<float>()) setHeaterDuty(p["heater_pwm"].as<float>());
    if (p["motor_pwm"].is<float>())  setMotorDuty(p["motor_pwm"].as<float>());
    if (p["acid_pwm"].is<float>())   setAcidDuty(p["acid_pwm"].as<float>());
    if (p["base_pwm"].is<float>())   setBaseDuty(p["base_pwm"].as<float>());
    sendControlAck(id, true);
    Serial.println("[cmd] actuator override applied");
  }
}

void readCommands() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n') {
      if (rxLine.length() > 0) {
        JsonDocument cmd;
        DeserializationError err = deserializeJson(cmd, rxLine);
        if (!err) applyCommand(cmd);
        rxLine = "";
      }
    } else if (rxLine.length() < 512) {
      rxLine += c;
    }
  }
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  Serial.begin(115200);        // USB debug
  Serial1.begin(UART_BAUD);    // UART to DevKitC bridge

  pinMode(PIN_TEMP_ADC, INPUT);
  pinMode(PIN_PH_ADC, INPUT);
  pinMode(PIN_RPM_SENSOR, INPUT_PULLUP);
  pinMode(PIN_HEATER, OUTPUT);
  pinMode(PIN_PUMP_ACID, OUTPUT);
  pinMode(PIN_PUMP_ALKALI, OUTPUT);
  pinMode(PIN_STIRRER_PWM, OUTPUT);
  digitalWrite(PIN_PUMP_ACID, LOW);
  digitalWrite(PIN_PUMP_ALKALI, LOW);

  pwmAttach(PIN_HEATER, LEDC_CH_HEATER, LEDC_FREQ, LEDC_RES);        // heater PWM (60% cap applied in setHeaterDuty)
  pwmAttach(PIN_STIRRER_PWM, LEDC_CH_STIRRER, LEDC_FREQ, LEDC_RES);  // stirrer PWM
  pwmWrite(PIN_HEATER, LEDC_CH_HEATER, 0);
  pwmWrite(PIN_STIRRER_PWM, LEDC_CH_STIRRER, 0);

  attachInterrupt(digitalPinToInterrupt(PIN_RPM_SENSOR), rpmISR, FALLING);

  resetWindow(millis());
  lastRpmMillis = millis();
  Serial.println("Nano ESP32 bioreactor controller booted.");
}

void loop() {
  unsigned long now = millis();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    float t = readTemperatureC();
    float p = readPH();
    float r = readRPM();
    controlLoop(t, p, r);
    accumulate(t, p, r);
  }

  readCommands();

  if (now - windowStart >= WINDOW_MS) {
    sendTelemetry(now);
    resetWindow(now);
  }

  delay(10);
}
