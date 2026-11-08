#pragma once

// ===== MODEM-PINS (Waveshare ESP32-S3-A7670E-4G) =====
// ACHTUNG: Diese Werte MUESSEN mit dem Waveshare-Wiki/Schaltplan deiner
// Platine (Hardware 2.0, Variante A) abgeglichen werden. Falsche Pins =
// "modem: nicht erreichbar" im Status. Nur hier aendern, sonst nirgends.
#define MODEM_TX_PIN     17   // ESP32 TX -> Modem RX   (PRUEFEN)
#define MODEM_RX_PIN     18   // ESP32 RX <- Modem TX   (PRUEFEN)
#define MODEM_PWRKEY_PIN -1   // -1 = nicht verwendet   (PRUEFEN)
#define MODEM_BAUD       115200

// ===== WLAN-HOTSPOT DER BOX =====
#define AP_SSID          "CYBER-BOX"
#define AP_DEFAULT_PASS  "cybertv2026"   // Beim ersten Start im Web-Menue aendern!

// ===== BLE =====
#define BLE_NAME         "CYBER-BOX"
#define BLE_SERVICE_UUID "c7b00001-0000-4c5e-9c1a-cb3a7e000001"
#define BLE_CMD_UUID     "c7b00002-0000-4c5e-9c1a-cb3a7e000001"  // Display -> Box (write)
#define BLE_STATUS_UUID  "c7b00003-0000-4c5e-9c1a-cb3a7e000001"  // Box -> Display (read/notify)

// ===== VERBINDUNGSLOGIK =====
#define CHECK_INTERVAL_MS   15000
#define WIFI_TIMEOUT_MS     15000
#define NETWORK_TIMEOUT_MS  60000
#define MAX_TVS             6
