/*
 * Bioreactor — TTGO T-Display (ESP32 + ST7789 135x240) OFFLINE monitor
 * =====================================================================
 * A small standalone display that shows the bioreactor's live readings WITHOUT
 * the laptop / server / dashboard. It is fully offline: no WiFi, no MQTT, no
 * server. It passively "taps" the same UART line the Nano controller already
 * uses to send telemetry to the DevKitC bridge, and renders the values on the
 * built-in 1.14" screen.
 *
 * Wiring — passive tap (keep the existing Nano <-> DevKitC link untouched):
 *   T-Display GND   ->  Nano GND
 *   T-Display GPIO27 -> Nano TX (D1)                    [telemetry, required]
 *   T-Display GPIO21 -> DevKitC TX2 (GPIO17)            [commands, optional — shows setpoints]
 *
 * It only LISTENS on these lines; it never transmits, so it cannot interfere
 * with the controller/bridge.
 *
 * Required libraries (Arduino Library Manager):
 *   - TFT_eSPI     by Bodmer
 *   - ArduinoJson  by Benoit Blanchon
 *
 * TFT_eSPI setup — open the library's User_Setup.h and use these values
 * (the standard TTGO T-Display pinout):
 *   #define ST7789_DRIVER
 *   #define TFT_WIDTH   135
 *   #define TFT_HEIGHT  240
 *   #define TFT_MISO  -1
 *   #define TFT_MOSI   19
 *   #define TFT_SCLK   18
 *   #define TFT_CS      5u
 *   #define TFT_DC     16
 *   #define TFT_RST    23
 *   #define TFT_BL      4
 *   #define LOAD_GLCD
 *   #define LOAD_FONT2
 *   #define LOAD_FONT4
 *   #define SMOOTH_FONT
 */

#include <TFT_eSPI.h>
#include <SPI.h>
#include <ArduinoJson.h>

TFT_eSPI tft = TFT_eSPI();

// UART tap pins (RX listens, TX is unused but must be a valid free pin).
#define RX_TELEMETRY 27
#define TX_TELEMETRY 25
#define RX_COMMANDS  21
#define TX_COMMANDS  33

const unsigned long BAUD     = 115200;
const unsigned long STALE_MS = 5000;   // show "NO DATA" after 5 s of silence

// Live values parsed from the two UART lines.
float temperature = NAN;
float ph = NAN;
float rpm = NAN;
float spTemp = NAN;
float spPh = NAN;
float spRpm = NAN;
bool heater = false, acid = false, alkali = false, stirrer = false;
unsigned long lastRx = 0;

String lineTelemetry = "";
String lineCommands = "";

// Last-drawn values (redraw only on change -> no flicker).
float dTemp = NAN, dPh = NAN, dRpm = NAN;
float dSpTemp = NAN, dSpPh = NAN, dSpRpm = NAN;
bool dHeater = false, dAcid = false, dAlkali = false, dStirrer = false;
bool dStale = true;

// ---------------------------------------------------------------- parsing
// Accepts BOTH the course telemetry shape (temperature_C/pH/rpm -> mean,
// actuators_avg -> *_pwm, setpoints) and the legacy flat shape.
void parseTelemetry(const String& s) {
  JsonDocument doc;
  if (deserializeJson(doc, s)) return;

  // New course shape: mean/min/max objects.
  if (doc["temperature_C"]["mean"].is<float>()) temperature = doc["temperature_C"]["mean"].as<float>();
  else if (doc["temperature"].is<float>())      temperature = doc["temperature"].as<float>();

  if (doc["pH"]["mean"].is<float>()) ph = doc["pH"]["mean"].as<float>();
  else if (doc["ph"].is<float>())    ph = doc["ph"].as<float>();

  if (doc["rpm"]["mean"].is<float>()) rpm = doc["rpm"]["mean"].as<float>();
  else if (doc["rpm"].is<float>())    rpm = doc["rpm"].as<float>();

  // Setpoints are carried in the telemetry (new shape).
  if (doc["setpoints"].is<JsonObject>()) {
    JsonObject sp = doc["setpoints"];
    if (sp["temperature_C"].is<float>()) spTemp = sp["temperature_C"].as<float>();
    if (sp["pH"].is<float>())            spPh = sp["pH"].as<float>();
    if (sp["rpm"].is<float>())           spRpm = sp["rpm"].as<float>();
  }

  // Actuators: new shape uses 0..1 duty averages.
  if (doc["actuators_avg"].is<JsonObject>()) {
    JsonObject act = doc["actuators_avg"];
    heater  = act["heater_pwm"] | false;
    acid    = act["acid_pwm"]   | false;
    alkali  = act["base_pwm"]   | false;
    stirrer = act["motor_pwm"]  | false;
  } else {
    heater  = doc["heater"] | false;
    acid    = doc["pump_acid"] | false;
    alkali  = doc["pump_alkali"] | false;
    stirrer = doc["stirrer"] | true;
  }

  lastRx = millis();
}

void parseCommand(const String& s) {
  JsonDocument doc;
  if (deserializeJson(doc, s)) return;

  const char* type = doc["type"] | "";
  if (String(type) == "setpoints") {
    JsonObject p = doc["payload"].as<JsonObject>();
    if (p["temperature_C"].is<float>()) spTemp = p["temperature_C"].as<float>();
    else if (p["temp"].is<float>())     spTemp = p["temp"].as<float>();
    if (p["pH"].is<float>()) spPh = p["pH"].as<float>();
    else if (p["ph"].is<float>()) spPh = p["ph"].as<float>();
    if (p["rpm"].is<float>()) spRpm = p["rpm"].as<float>();
  }
}

// Read complete newline-terminated lines from a UART and dispatch them.
void pump(HardwareSerial& port, String& buf, bool isTelemetry) {
  while (port.available()) {
    char c = (char)port.read();
    if (c == '\n') {
      if (buf.length() > 0) {
        if (isTelemetry) parseTelemetry(buf);
        else             parseCommand(buf);
        buf = "";
      }
    } else if (buf.length() < 512) {
      buf += c;
    }
  }
}

// ---------------------------------------------------------------- drawing
void drawValue(int cx, float v, int dec, float sp, int spDec, float* lastV, float* lastSp) {
  if (v == *lastV && sp == *lastSp) return;   // unchanged
  *lastV = v;
  *lastSp = sp;

  tft.setTextDatum(TC_DATUM);
  tft.setTextPadding(80);

  // current value (big)
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (isnan(v)) tft.drawString("--", cx, 44, 4);
  else          tft.drawString(String(v, dec), cx, 44, 4);

  // setpoint (small)
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  if (isnan(sp)) tft.drawString("SP --", cx, 76, 2);
  else {
    String s = "SP ";
    s += String(sp, spDec);
    tft.drawString(s, cx, 76, 2);
  }
}

void drawPill(int x, int y, int w, int h, const char* label, bool on, uint16_t onColor) {
  tft.fillRoundRect(x, y, w, h, 4, on ? onColor : TFT_DARKGREY);
  tft.drawRoundRect(x, y, w, h, 4, on ? onColor : TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(on ? TFT_WHITE : TFT_LIGHTGREY, on ? onColor : TFT_DARKGREY);
  tft.setTextPadding(w);
  tft.drawString(label, x + w / 2, y + h / 2, 2);
}

void render() {
  bool stale = (millis() - lastRx) > STALE_MS;

  if (stale != dStale) {
    dStale = stale;
    uint16_t col = stale ? TFT_RED : TFT_GREEN;
    tft.fillCircle(228, 7, 4, col);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(col, TFT_NAVY);
    tft.setTextPadding(70);
    tft.drawString(stale ? "NO DATA" : "LIVE", 222, 4, 1);
  }

  drawValue(40,  temperature, 1, spTemp, 1, &dTemp, &dSpTemp);
  drawValue(120, ph, 2, spPh, 1, &dPh, &dSpPh);
  drawValue(200, rpm, 0, spRpm, 0, &dRpm, &dSpRpm);

  if (heater  != dHeater)  { dHeater  = heater;  drawPill(4,   92, 112, 16, "HEATER",  heater,  TFT_RED); }
  if (acid    != dAcid)    { dAcid    = acid;    drawPill(124, 92, 112, 16, "ACID",    acid,    TFT_ORANGE); }
  if (alkali  != dAlkali)  { dAlkali  = alkali;  drawPill(4,   112, 112, 16, "ALKALI",  alkali,  TFT_BLUE); }
  if (stirrer != dStirrer) { dStirrer = stirrer; drawPill(124, 112, 112, 16, "STIRRER", stirrer, TFT_CYAN); }
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  Serial.begin(115200);

  // Passive UART taps (listen only).
  Serial1.begin(BAUD, SERIAL_8N1, RX_TELEMETRY, TX_TELEMETRY);
  Serial2.begin(BAUD, SERIAL_8N1, RX_COMMANDS, TX_COMMANDS);

  tft.init();
  tft.setRotation(1);            // landscape 240 x 135
  tft.fillScreen(TFT_BLACK);

  // Header
  tft.fillRect(0, 0, 240, 14, TFT_NAVY);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("BIOREACTOR", 4, 3, 1);

  // Column labels
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK); tft.drawString("TEMP", 40,  18, 2);
  tft.setTextColor(TFT_GREEN,  TFT_BLACK); tft.drawString("pH",   120, 18, 2);
  tft.setTextColor(TFT_CYAN,   TFT_BLACK); tft.drawString("RPM",  200, 18, 2);

  Serial.println("TTGO T-Display offline monitor ready.");
}

void loop() {
  pump(Serial1, lineTelemetry, true);
  pump(Serial2, lineCommands, false);
  render();
  delay(20);
}
