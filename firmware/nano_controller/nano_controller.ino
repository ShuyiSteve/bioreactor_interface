/*
 * Bioreactor — Arduino Nano ESP32 (ESP32-S3) controller
 * ======================================================
 * This board sits next to the bioreactor hardware and is responsible for the
 * local control loop and for talking to the sensors/actuators directly
 * (wired analogue/digital signals, per the Connectivity spec).
 *
 * It does NOT connect to WiFi. Instead it exchanges line-delimited JSON with
 * the DevKitC bridge over UART:
 *
 *   Nano -> Bridge   (telemetry, every 1 s):
 *     {"temperature":29.5,"ph":5.1,"rpm":1002,"heater":1,"pump_acid":0,"pump_alkali":0,"stirrer":1}
 *
 *   Bridge -> Nano   (commands, applied on receipt):
 *     {"type":"setpoints","payload":{"temp":30,"ph":5,"rpm":1000,"mode":"auto"}}
 *     {"type":"control","payload":{"heater":1,"pump_acid":0,"pump_alkali":0,"stirrer":1}}
 *
 * Wiring to the DevKitC (UART, crossed):
 *   Nano TX (D1)  -> DevKit RX2 (GPIO16)
 *   Nano RX (D0)  <- DevKit TX2 (GPIO17)
 *   GND           <-> GND
 *
 * Required libraries: ArduinoJson (Benoit Blanchon)
 * Board package: Arduino Nano ESP32 (supported by default in the Arduino IDE)
 */

#include <ArduinoJson.h>

// ---------------------------------------------------------------- pin map
const int PIN_TEMP_ADC    = A0;  // thermistor divider (10 k NTC + 10 k series)
const int PIN_PH_ADC      = A1;  // pH probe conditioning board
const int PIN_RPM_SENSOR  = 2;   // IR reflective sensor (digital, 2 pulses/rev)
const int PIN_HEATER      = 3;   // heater relay / MOSFET (3 Ohm, 30 W)
const int PIN_PUMP_ACID   = 4;   // acid peristaltic pump
const int PIN_PUMP_ALKALI = 5;   // alkali peristaltic pump
const int PIN_STIRRER_PWM = 6;   // stirring motor (PWM)

// UART to the DevKitC bridge.
const unsigned long UART_BAUD = 115200;
const unsigned long TX_MS     = 1000;   // send telemetry every 1 s

// Thermistor (10 kOhm NTC, beta model).
const float R_REF  = 10000.0f;
const float R_NOM  = 10000.0f;
const float B_COEF = 3950.0f;
const float T_NOM  = 298.15f;

// pH calibration — CALIBRATE WITH BUFFER SOLUTIONS.
const float PH_SLOPE  = -5.70f;
const float PH_OFFSET = 21.34f;

// Control setpoints (overwritten by commands from the dashboard).
float setpointTemp = 30.0f;
float setpointPh   = 5.0f;
float setpointRpm  = 1000.0f;
String mode        = "auto";

// Actuator states.
bool heater = false;
bool pumpAcid = false;
bool pumpAlkali = false;
bool stirrer = true;

// RPM measurement.
volatile unsigned long pulseCount = 0;
unsigned long lastRpmMillis = 0;
float measuredRpm = 0.0f;

unsigned long lastTx = 0;
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
  measuredRpm = (pulseCount * 60.0f) / 2.0f / elapsed;  // 2 pulses / rev
  pulseCount = 0;
  lastRpmMillis = now;
  return measuredRpm;
}

// ---------------------------------------------------------------- actuators
void setHeater(bool on) { heater = on; digitalWrite(PIN_HEATER, on ? HIGH : LOW); }
void setAcid(bool on)   { pumpAcid = on; digitalWrite(PIN_PUMP_ACID, on ? HIGH : LOW); }
void setAlkali(bool on) { pumpAlkali = on; digitalWrite(PIN_PUMP_ALKALI, on ? HIGH : LOW); }

void setStirrerPWM(float rpmTarget) {
  if (!stirrer || rpmTarget <= 0) { ledcWrite(0, 0); return; }
  int duty = (int)constrain(map(rpmTarget, 500.0f, 1500.0f, 90.0f, 255.0f), 0, 255);
  ledcWrite(0, duty);
}

void controlLoop() {
  float temp = readTemperatureC();
  float ph = readPH();
  float rpm = readRPM();

  if (mode == "auto") {
    if (temp < setpointTemp - 0.25f) setHeater(true);
    else if (temp > setpointTemp + 0.25f) setHeater(false);

    if (ph > setpointPh + 0.1f) { setAcid(true); setAlkali(false); }
    else if (ph < setpointPh - 0.1f) { setAlkali(true); setAcid(false); }
    else { setAcid(false); setAlkali(false); }

    stirrer = true;
  }

  setStirrerPWM(stirrer ? setpointRpm : 0.0f);
}

// ---------------------------------------------------------------- UART link
void sendTelemetry() {
  JsonDocument doc;
  doc["temperature"] = readTemperatureC();
  doc["ph"] = readPH();
  doc["rpm"] = readRPM();
  doc["heater"] = heater;
  doc["pump_acid"] = pumpAcid;
  doc["pump_alkali"] = pumpAlkali;
  doc["stirrer"] = stirrer;

  serializeJson(doc, Serial1);
  Serial1.print('\n');
}

void applyCommand(const JsonDocument& cmd) {
  const char* type = cmd["type"] | "";
  JsonObject p = cmd["payload"].as<JsonObject>();

  if (String(type) == "setpoints") {
    if (p["temp"].is<float>()) setpointTemp = p["temp"].as<float>();
    if (p["ph"].is<float>())   setpointPh   = p["ph"].as<float>();
    if (p["rpm"].is<float>())  setpointRpm  = p["rpm"].as<float>();
    if (p["mode"].is<const char*>()) mode = p["mode"].as<const char*>();
    Serial.printf("[cmd] setpoints %.1f C / pH %.1f / %.0f RPM / %s\n",
                  setpointTemp, setpointPh, setpointRpm, mode.c_str());
  } else if (String(type) == "control") {
    if (p["heater"].is<bool>())      setHeater(p["heater"].as<bool>());
    if (p["pump_acid"].is<bool>())   setAcid(p["pump_acid"].as<bool>());
    if (p["pump_alkali"].is<bool>()) setAlkali(p["pump_alkali"].as<bool>());
    if (p["stirrer"].is<bool>())     stirrer = p["stirrer"].as<bool>();
    Serial.println("[cmd] actuator override");
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
  digitalWrite(PIN_HEATER, LOW);
  digitalWrite(PIN_PUMP_ACID, LOW);
  digitalWrite(PIN_PUMP_ALKALI, LOW);

  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_STIRRER_PWM, 0);
  attachInterrupt(digitalPinToInterrupt(PIN_RPM_SENSOR), rpmISR, FALLING);

  lastRpmMillis = millis();
  Serial.println("Nano ESP32 bioreactor controller booted.");
}

void loop() {
  controlLoop();
  readCommands();

  if (millis() - lastTx >= TX_MS) {
    lastTx = millis();
    sendTelemetry();
  }
  delay(10);
}
