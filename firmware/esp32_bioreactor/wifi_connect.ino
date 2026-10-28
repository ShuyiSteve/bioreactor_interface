// configure your connection here
#define USE_EDUROAM 1
#define WIFI_USER "XXXXXX@ucl.ac.uk"
#define WIFI_SSID "eduroam"
#define WIFI_PASS "your_password"
// end of configuration
#include "esp_sntp.h"
#include "WiFi.h"
#include <TFT_eSPI.h>
#include <SPI.h>
#if USE_EDUROAM
#include "esp_eap_client.h"
const char* ssid = "eduroam";
TFT_eSPI tft = TFT_eSPI();
const char* user = WIFI_USER;
#else
const char* ssid = WIFI_SSID;
#endif

const char* pass = WIFI_PASS;
const char* ntpServer = "pool.ntp.org";
bool digital;
double passMinsx;
double passMinsy;
float sx = 0, sy = 1, mx = 1, my = 0, hx = -1, hy = 0; 
float pastHours1, pastHours2, pastMins1, pastMins2, pastSecs1, pastSecs2; 
uint16_t x0=0, x1=0, yy0=0, yy1=0;


// dump some details of the WiFi connection (if any) to the serial line
void print_wifi_info() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected to WiFi network: ");
    Serial.println(WiFi.SSID());
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Gateway address: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("Subnet mask: ");
    Serial.println(WiFi.subnetMask());
    Serial.print("MAC address: ");
    Serial.println(WiFi.macAddress());
  } else {
    Serial.println("WiFi not connected");
    Serial.print("MAC address: ");
    Serial.println(WiFi.macAddress());
  }
}

void wifi_connect(float timeout = 15) {
  unsigned long deadline;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);

#if USE_EDUROAM
  Serial.printf("Connecting to eduroam as user %s\n", user);
  esp_eap_client_set_identity((uint8_t*)user, strlen(user));
  esp_eap_client_set_username((uint8_t*)user, strlen(user));
  esp_eap_client_set_password((uint8_t*)pass, strlen(pass));
  esp_wifi_sta_enterprise_enable();
  WiFi.begin(ssid);
#else
  Serial.printf("Connecting to %s\n", ssid);
  WiFi.begin(ssid, pass);
#endif

  deadline = millis() + (unsigned long)(timeout * 1000);

  while ((WiFi.status() != WL_CONNECTED) && (millis() < deadline)) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  print_wifi_info();
}

void setup()  //sets the time up
{
  Serial.begin(115200);
  wifi_connect();

}