#include <Arduino.h>
#include <WiFi.h>

void setup() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("CYBER-TEST", "12345678");
}

void loop() {
  delay(1000);
}
