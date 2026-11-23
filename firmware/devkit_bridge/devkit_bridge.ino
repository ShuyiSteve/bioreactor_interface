/*
 * Bioreactor — ESP32 DevKitC V4 (ESP32-WROVER) network bridge
 * =============================================================
 * Bridges the Nano controller to the outside world over WiFi using MQTT.
 *
 * UART side (to the Nano controller, crossed):
 *   DevKit RX2 (GPIO16) <- Nano TX (D1)
 *   DevKit TX2 (GPIO17) -> Nano RX (D0)
 *   GND                  <-> GND
 *
 * MQTT side:
 *   publish   -> bioreactor/<DEVICE_ID>/telemetry   (adds NTP window times + ts)
 *   publish   -> bioreactor/<DEVICE_ID>/ack         (forwards Nano's {id, ok, applied})
 *   publish   -> bioreactor/<DEVICE_ID>/status      ("online" on connect)
 *   subscribe -> bioreactor/<DEVICE_ID>/commands
 *
 * Team spec:
 *   item 4 — status topic is the MQTT last will ("offline"); "online" on connect
 *   item 6 — if the link drops, telemetry is buffered and resent (with its seq)
 *            once the connection comes back
 *
 * Required libraries:
 *   - PicoMQTT  (recommended by the spec; Library Manager)
 *   - ArduinoJson
 * Board package: esp32 by Espressif Systems
 */

#include <WiFi.h>
#include <PicoMQTT.h>
#include <ArduinoJson.h>
#include <sys/time.h>

// ---------------------------------------------------------------- config
// WiFi credentials (SSID / password) live in `secret.h`, which is gitignored
// so nobody accidentally commits their real credentials.
#include "secret.h"
#ifndef USE_EDUROAM
#define USE_EDUROAM 0   // older secret.h files without the eduroam option
#endif
#if USE_EDUROAM
#include "esp_eap_client.h"
#endif

// Public brokers suggested by the spec:
//   broker.hivemq.com      (public, no auth)
//   test.mosquitto.org     (public, no auth)
//   engf0001.cs.ucl.ac.uk  (UCL-only; use from the CS network)
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;

const char* DEVICE_ID = "bio1";       // must match MQTT_DEVICE_ID on the server
const char* TOPIC     = "bioreactor"; // must match MQTT_TOPIC on the server

// UART to the Nano controller.
const unsigned long UART_BAUD = 115200;

WiFiClient wifiClient;
PicoMQTT::Client mqtt(wifiClient, MQTT_BROKER, MQTT_PORT);

String txLine = "";
String cmdTopic, telTopic, ackTopic, statusTopic;

// Telemetry buffer for link drops (item 6): store samples while MQTT is down,
// then resend them (original seq + timestamps preserved) on reconnect.
const int TELEMETRY_BUFFER_SIZE = 120;   // ~2 min at 1 Hz
String telemetryBuffer[TELEMETRY_BUFFER_SIZE];
int bufferHead = 0;
int bufferCount = 0;
volatile bool mqttJustConnected = false;

// ---------------------------------------------------------------- wifi
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
#if USE_EDUROAM
  // Same approach as the Moodle "Connecting to eduroam" example (Week 4).
  Serial.printf("Connecting to eduroam as %s", WIFI_USER);
  esp_eap_client_set_identity((uint8_t*)WIFI_USER, strlen(WIFI_USER));
  esp_eap_client_set_username((uint8_t*)WIFI_USER, strlen(WIFI_USER));
  esp_eap_client_set_password((uint8_t*)WIFI_PASS, strlen(WIFI_PASS));
  esp_wifi_sta_enterprise_enable();
  WiFi.begin(WIFI_SSID);
#else
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#endif
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("============================================");
    Serial.println("  WiFi connected successfully!");
    Serial.print("  SSID      : ");
    Serial.println(WiFi.SSID());
    Serial.print("  IP address: ");
    Serial.println(WiFi.localIP());
    Serial.println("============================================");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("  NTP sync requested.");
  } else {
    Serial.println();
    Serial.println("WiFi connection failed.");
  }
}

// Unix time in milliseconds (0 until NTP has synced).
unsigned long long unixMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1600000000) return 0;   // not synced yet
  return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)(tv.tv_usec / 1000);
}

// ---------------------------------------------------------------- mqtt
// A command arrived from the server; forward it verbatim to the Nano over UART.
void onCommand(const char* topic, const char* payload) {
  Serial2.print(payload);
  Serial2.print('\n');
  Serial.printf("[mqtt] forwarded command to Nano\n");
}

// ---------------------------------------------------------------- buffering
void bufferTelemetry(const String& line) {
  if (bufferCount < TELEMETRY_BUFFER_SIZE) {
    telemetryBuffer[(bufferHead + bufferCount) % TELEMETRY_BUFFER_SIZE] = line;
    bufferCount++;
  } else {
    // Full: drop the oldest sample to make room for the newest.
    telemetryBuffer[bufferHead] = line;
    bufferHead = (bufferHead + 1) % TELEMETRY_BUFFER_SIZE;
  }
}

void flushTelemetryBuffer() {
  int n = bufferCount;
  while (bufferCount > 0) {
    String line = telemetryBuffer[bufferHead];
    bufferHead = (bufferHead + 1) % TELEMETRY_BUFFER_SIZE;
    bufferCount--;
    mqtt.publish(telTopic.c_str(), line.c_str());
  }
  if (n > 0) Serial.printf("[mqtt] flushed %d buffered telemetry samples\n", n);
}

// ---------------------------------------------------------------- uart -> mqtt
// Route a line from the Nano: telemetry (has "seq") or ack (has "id"+"ok").
void handleNanoLine(const String& line) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) return;

  if (doc.containsKey("seq")) {
    // Telemetry: stamp the unix timestamps from NTP before publishing.
    // window.start/end are unix seconds (as in the agreed format); ts is the
    // same end time in unix ms, which the server stores.
    unsigned long long ts = unixMs();
    if (ts != 0) {
      doc["ts"] = (int64_t)ts;
      JsonObject win = doc["window"].as<JsonObject>();
      if (!win.isNull()) {
        double secs = win["seconds"] | 1.0;
        win["end"] = ts / 1000.0;
        win["start"] = ts / 1000.0 - secs;
      }
    }
    String out;
    serializeJson(doc, out);

    if (mqtt.connected()) {
      mqtt.publish(telTopic.c_str(), out.c_str());
    } else {
      bufferTelemetry(out);   // link down: buffer and resend later (item 6)
    }
  } else if (doc.containsKey("id") && doc.containsKey("ok")) {
    // Command acknowledgement from the Nano (only useful while connected).
    if (mqtt.connected()) mqtt.publish(ackTopic.c_str(), line.c_str());
  }
}

void readNano() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\n') {
      if (txLine.length() > 0) {
        handleNanoLine(txLine);
        txLine = "";
      }
    } else if (txLine.length() < 1024) {
      txLine += c;
    }
  }
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  Serial.begin(115200);     // USB debug
  Serial2.begin(UART_BAUD); // UART to Nano (RX2=GPIO16, TX2=GPIO17)

  cmdTopic = String(TOPIC) + "/" + DEVICE_ID + "/commands";
  telTopic = String(TOPIC) + "/" + DEVICE_ID + "/telemetry";
  ackTopic = String(TOPIC) + "/" + DEVICE_ID + "/ack";
  statusTopic = String(TOPIC) + "/" + DEVICE_ID + "/status";

  // Item 4: status topic via MQTT last will. The broker publishes "offline" on
  // our behalf if we drop unexpectedly; we publish "online" on connect.
  mqtt.will.topic = statusTopic;
  mqtt.will.payload = "offline";
  mqtt.will.qos = 0;
  mqtt.will.retain = true;

  mqtt.connected_callback = [] {
    mqttJustConnected = true;
    Serial.println("[mqtt] connected");
  };
  mqtt.disconnected_callback = [] {
    Serial.println("[mqtt] disconnected (will publish offline via LWT)");
  };

  mqtt.subscribe(cmdTopic, onCommand);

  connectWiFi();
  mqtt.begin();
  Serial.println("DevKitC bridge booted.");
}

void loop() {
  mqtt.loop();   // PicoMQTT auto-reconnects and re-subscribes

  // On (re)connect: announce online + flush buffered telemetry (items 4 + 6).
  if (mqttJustConnected) {
    mqttJustConnected = false;
    mqtt.publish(statusTopic.c_str(), "online");
    flushTelemetryBuffer();
  }

  readNano();
  delay(5);
}
