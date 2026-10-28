/*
 * Connected Bioreactor - ESP32 firmware (single-board, HTTP)
 * ===========================================================
 * Monitors temperature (10 kOhm NTC thermistor), pH (analog probe) and
 * stirring speed (IR reflective sensor, 2 pulses/rev), runs the control loop,
 * and exchanges data with the Node.js server over WiFi (HTTP).
 *
 * WiFi supports both normal WPA/WPA2 and eduroam (WPA2-Enterprise / EAP):
 * set USE_EDUROAM to 1 below and fill in your UCL credentials.
 *
 * Required libraries: ArduinoJson (Benoit Blanchon)
 * Board package: esp32 by Espressif Systems
 */

// ------------------------------------------------------------------ config
// WiFi: 0 = normal WPA/WPA2 home WiFi, 1 = eduroam (WPA2-Enterprise / EAP)
#define USE_EDUROAM 0

// eduroam / UCL credentials (used when USE_EDUROAM is 1).
// Phase-1 (outer) and phase-2 (inner) identity are both set to this user,
// matching the UCL Moodle example. Change WIFI_USER to an anonymous identity
// (e.g. "anonymous@ucl.ac.uk") if your campus requires it for the outer phase.
#define WIFI_USER "XXXXXX@ucl.ac.uk"

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

#include <string.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#if USE_EDUROAM
#include "esp_eap_client.h"
#endif

// IP/port of the machine running `node backend/server.js`.
// Use your computer's LAN IP (e.g. http://192.168.1.50:3001), NOT localhost.
const char* SERVER = "http://192.168.1.50:3001";

// Pin map — adjust to your wiring.
const int PIN_TEMP_ADC     = 34;  // thermistor divider
const int PIN_PH_ADC       = 35;  // pH probe conditioning board
const int PIN_RPM_SENSOR   = 32;  // IR reflective sensor (digital)
const int PIN_HEATER       = 26;  // heater relay / MOSFET (3 Ohm, 30 W)
const int PIN_PUMP_ACID    = 27;  // acid peristaltic pump
const int PIN_PUMP_ALKALI  = 14;  // alkali peristaltic pump
const int PIN_STIRRER_PWM  = 25;  // stirring motor (PWM)

// Thermistor (10 kOhm NTC, beta model).
const float R_REF  = 10000.0f;   // series resistor (ohms)
const float R_NOM  = 10000.0f;   // thermistor resistance at 25 C
const float B_COEF = 3950.0f;    // beta coefficient
const float T_NOM  = 298.15f;    // 25 C in Kelvin

// pH calibration — CALIBRATE WITH BUFFER SOLUTIONS.
// pH = PH_SLOPE * probeVoltage + PH_OFFSET
const float PH_SLOPE  = -5.70f;
const float PH_OFFSET = 21.34f;

// Control setpoints (overwritten by the server, defaults match the spec).
float setpointTemp = 30.0f;
float setpointPh   = 5.0f;
float setpointRpm  = 1000.0f;
String mode        = "auto";   // "auto" or "manual"

// Actuator states.
bool heater = false;
bool pumpAcid = false;
bool pumpAlkali = false;
bool stirrer = true;

// RPM measurement.
volatile unsigned long pulseCount = 0;
unsigned long lastRpmMillis = 0;
float measuredRpm = 0.0f;

// Timing.
const unsigned long INGEST_MS   = 1000;  // upload a reading every 1 s
const unsigned long COMMAND_MS  = 3000;
unsigned long lastIngest  = 0;
unsigned long lastCommand = 0;

void IRAM_ATTR rpmISR() {
  pulseCount++;
}

// ------------------------------------------------------------------ sensors
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
  // 2 pulses per revolution.
  measuredRpm = (pulseCount * 60.0f) / 2.0f / elapsed;
  pulseCount = 0;
  lastRpmMillis = now;
  return measuredRpm;
}

// ------------------------------------------------------------------ control
void setHeater(bool on)  { heater = on;  digitalWrite(PIN_HEATER, on ? HIGH : LOW); }
void setAcid(bool on)    { pumpAcid = on;  digitalWrite(PIN_PUMP_ACID, on ? HIGH : LOW); }
void setAlkali(bool on)  { pumpAlkali = on; digitalWrite(PIN_PUMP_ALKALI, on ? HIGH : LOW); }

void setStirrerPWM(float rpmTarget) {
  // Open-loop proportional map 500..1500 RPM -> PWM duty (tune to your motor).
  if (!stirrer || rpmTarget <= 0) {
    ledcWrite(0, 0);
    return;
  }
  int duty = (int)constrain(map(rpmTarget, 500.0f, 1500.0f, 90.0f, 255.0f), 0, 255);
  ledcWrite(0, duty);
}

void controlLoop() {
  float temp = readTemperatureC();
  float ph = readPH();
  float rpm = readRPM();

  if (mode == "auto") {
    // Hysteresis (temperature: +/- 0.25 C, pH: +/- 0.1).
    if (temp < setpointTemp - 0.25f) setHeater(true);
    else if (temp > setpointTemp + 0.25f) setHeater(false);

    if (ph > setpointPh + 0.1f) { setAcid(true);  setAlkali(false); }
    else if (ph < setpointPh - 0.1f) { setAlkali(true); setAcid(false); }
    else { setAcid(false); setAlkali(false); }

    stirrer = true;
  }

  setStirrerPWM(stirrer ? setpointRpm : 0.0f);
}

// ------------------------------------------------------------------ network
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);

#if USE_EDUROAM
  Serial.printf("Connecting to eduroam as %s\n", WIFI_USER);
  esp_eap_client_set_identity((uint8_t*)WIFI_USER, strlen(WIFI_USER));
  esp_eap_client_set_username((uint8_t*)WIFI_USER, strlen(WIFI_USER));
  esp_eap_client_set_password((uint8_t*)WIFI_PASS, strlen(WIFI_PASS));
  esp_wifi_sta_enterprise_enable();
  WiFi.begin(WIFI_SSID);
#else
  Serial.printf("Connecting to %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#endif

  // eduroam EAP handshakes can take longer than a home network.
  unsigned long deadline = millis() + 20000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connection failed.");
  }
}

void applyCommands(JsonArray commands) {
  for (JsonObject cmd : commands) {
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
      Serial.println("[cmd] actuator override applied");
    }
  }
}

void ingest() {
  float temp = readTemperatureC();
  float ph = readPH();
  float rpm = readRPM();

  JsonDocument doc;
  doc["temperature"] = temp;
  doc["ph"] = ph;
  doc["rpm"] = rpm;
  doc["heater"] = heater;
  doc["pump_acid"] = pumpAcid;
  doc["pump_alkali"] = pumpAlkali;
  doc["stirrer"] = stirrer;

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(String(SERVER) + "/api/ingest");
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code == 200) {
    JsonDocument resp;
    DeserializationError err = deserializeJson(resp, http.getString());
    if (!err && resp["commands"].is<JsonArray>()) {
      applyCommands(resp["commands"].as<JsonArray>());
    }
  } else {
    Serial.printf("[ingest] HTTP %d\n", code);
  }
  http.end();
}

void pollCommands() {
  HTTPClient http;
  http.begin(String(SERVER) + "/api/commands/pending");
  int code = http.GET();
  if (code == 200) {
    JsonDocument resp;
    DeserializationError err = deserializeJson(resp, http.getString());
    if (!err && resp["commands"].is<JsonArray>()) {
      applyCommands(resp["commands"].as<JsonArray>());
    }
  } else {
    Serial.printf("[poll] HTTP %d\n", code);
  }
  http.end();
}

// ------------------------------------------------------------------ setup/loop
void setup() {
  Serial.begin(115200);
  delay(300);

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

  ledcSetup(0, 5000, 8);   // PWM channel 0, 5 kHz, 8-bit
  ledcAttachPin(PIN_STIRRER_PWM, 0);
  attachInterrupt(digitalPinToInterrupt(PIN_RPM_SENSOR), rpmISR, FALLING);

  Serial.println("Bioreactor ESP32 booting…");
  connectWiFi();
  lastRpmMillis = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  controlLoop();

  unsigned long now = millis();
  if (now - lastIngest >= INGEST_MS) {
    lastIngest = now;
    ingest();
  }
  if (now - lastCommand >= COMMAND_MS) {
    lastCommand = now;
    pollCommands();
  }
  delay(20);
}
