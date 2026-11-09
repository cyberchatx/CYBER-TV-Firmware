#include <Arduino.h>
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println("START");
  WiFi.mode(WIFI_AP_STA);
  bool ok = WiFi.softAP("CYBER-TEST", "12345678");
  Serial.printf("softAP ok=%d IP=%s\n", ok, WiFi.softAPIP().toString().c_str());
  int n = WiFi.scanNetworks();
  Serial.printf("Gefundene WLANs: %d\n", n);
  for (int i = 0; i < n && i < 5; i++) Serial.println(WiFi.SSID(i));
}

void loop() {
  Serial.printf("laeuft %lu s, Clients %d\n", millis() / 1000, WiFi.softAPgetStationNum());
  delay(2000);
}
