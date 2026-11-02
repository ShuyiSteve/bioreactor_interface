/*
 * Bioreactor — ESP32 DevKitC V4 (ESP32-WROVER) network bridge
 * =============================================================
 * This board bridges the Nano controller to the outside world over WiFi using
 * MQTT (the approach recommended in the Connectivity spec).
 *
 * UART side (to the Nano controller, crossed):
 *   DevKit RX2 (GPIO16) <- Nano TX (D1)
 *   DevKit TX2 (GPIO17) -> Nano RX (D0)
 *   GND                  <-> GND
 *
 * MQTT side:
 *   publish  -> bioreactor/<DEVICE_ID>/telemetry   (JSON, one line per sample)
 *   subscribe-> bioreactor/<DEVICE_ID>/commands    (JSON command from the server)
 *
 * Required libraries:
 *   - PicoMQTT  (recommended by the spec; Library Manager)
 *   - ArduinoJson
 * Board package: esp32 by Espressif Systems
 */

#include <WiFi.h>
#include <PicoMQTT.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------- config
// WiFi credentials (SSID / password) live in `secret.h`, which is gitignored
// so nobody accidentally commits their real credentials.
#include "secret.h"

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
PicoMQTT::Client mqtt(MQTT_BROKER, MQTT_PORT, wifiClient);

String txLine = "";   // JSON line arriving from the Nano
String cmdTopic = "";
String telTopic = "";
unsigned long lastReconnect = 0;

// ---------------------------------------------------------------- wifi
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
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
  } else {
    Serial.println();
    Serial.println("WiFi connection failed.");
  }
}

// ---------------------------------------------------------------- mqtt
// A command arrived from the server; forward it verbatim to the Nano over UART.
void onCommand(const char* payload, size_t len) {
  Serial2.write((const uint8_t*)payload, len);
  Serial2.print('\n');
  Serial.printf("[mqtt] forwarded command to Nano (%u bytes)\n", (unsigned)len);
}

void connectMqtt() {
  if (mqtt.connected()) return;
  Serial.printf("Connecting MQTT broker %s:%d ...\n", MQTT_BROKER, MQTT_PORT);
  mqtt.begin();
  if (mqtt.connected()) {
    mqtt.subscribe(cmdTopic.c_str(), onCommand);
    Serial.printf("[mqtt] subscribed to %s\n", cmdTopic.c_str());
  } else {
    Serial.println("[mqtt] connection failed (will retry)");
  }
}

// ---------------------------------------------------------------- uart -> mqtt
void readNano() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\n') {
      if (txLine.length() > 0) {
        mqtt.publish(telTopic.c_str(), txLine.c_str());
        txLine = "";
      }
    } else if (txLine.length() < 512) {
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

  connectWiFi();
  connectMqtt();
  Serial.println("DevKitC bridge booted.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  // Retry MQTT every 5 s if it drops.
  if (!mqtt.connected() && millis() - lastReconnect > 5000) {
    lastReconnect = millis();
    connectMqtt();
  }

  mqtt.loop();
  readNano();
  delay(5);
}
