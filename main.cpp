// CYBER TV BOX - Hauptbox-Firmware v0.1
// Verbindungsreihenfolge: 4G -> Heim-WLAN -> nur eigener Hotspot "CYBER-BOX"
// Dienste: Web-Menue (Port 80), BLE-Steuerung, Verwaltung von bis zu 6 TVs.

#define TINY_GSM_MODEM_SIM7600   // A7670E ist AT-kompatibel zur SIM7600-Familie
#define TINY_GSM_RX_BUFFER 1024

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <TinyGsmClient.h>
#include <NimBLEDevice.h>
#include "config.h"

// ---------- Globale Objekte ----------
HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
WebServer server(80);
Preferences prefs;

NimBLECharacteristic* bleStatus = nullptr;

// ---------- Zustand (vom netTask geschrieben, vom Web/BLE gelesen) ----------
volatile bool g_online = false;
String g_mode = "start";        // 4g | wifi | hotspot | start
String g_operator = "";
String g_apnUsed = "";
String g_ip = "";
volatile int g_signal = 0;      // 0-31 (CSQ)
volatile bool g_modemOk = false;
volatile bool g_reconnect = true;
String g_lastCmd = "";
String g_plmn = "";             // z.B. 22801 = Swisscom
String g_provider = "";         // Swisscom | Sunrise | Salt | ...
String g_balance = "";          // z.B. "12.50" (CHF)
String g_balanceMsg = "";       // Originaltext der Netz-Antwort
uint32_t g_balanceAt = 0;       // millis() der letzten Abfrage
volatile bool g_balanceReq = false;

// ---------- Hilfsfunktionen ----------
String statusJson() {
  JsonDocument d;
  d["mode"] = g_mode;
  d["online"] = (bool)g_online;
  d["operator"] = g_operator;
  d["apn"] = g_apnUsed;
  d["ip"] = g_ip;
  d["signal"] = (int)g_signal;
  d["modem"] = (bool)g_modemOk;
  d["ap"] = AP_SSID;
  d["ap_clients"] = WiFi.softAPgetStationNum();
  d["lastCmd"] = g_lastCmd;
  d["plmn"] = g_plmn;
  d["provider"] = g_provider;
  d["balance_chf"] = g_balance;
  d["balance_msg"] = g_balanceMsg;
  d["balance_age_s"] = g_balanceAt ? (int)((millis() - g_balanceAt) / 1000) : -1;
  String out;
  serializeJson(d, out);
  return out;
}

void notifyStatus() {
  if (!bleStatus) return;
  String s = statusJson();
  bleStatus->setValue((uint8_t*)s.c_str(), s.length() > 500 ? 500 : s.length());
  bleStatus->notify();
}

// APN-Kandidaten je Anbieter (muss mit echten SIM-Karten getestet werden)
void apnCandidates(const String& op, String out[], int& n) {
  n = 0;
  String o = op; o.toLowerCase();
  String custom = prefs.getString("apn", "");
  if (custom.length()) out[n++] = custom;
  if (o.indexOf("swisscom") >= 0 || g_plmn == "22801") out[n++] = "gprs.swisscom.ch";
  if (o.indexOf("sunrise") >= 0) out[n++] = "internet";
  if (o.indexOf("salt") >= 0) out[n++] = "internet";
  // Fallbacks (MVNOs wie Lidl Connect, Aldi, Digital Republic)
  const char* fb[] = {"internet", "gprs.swisscom.ch", "internet.sunrise.ch"};
  for (auto f : fb) {
    bool dup = false;
    for (int i = 0; i < n; i++) if (out[i] == f) dup = true;
    if (!dup && n < 8) out[n++] = f;
  }
}

bool internetReachable() {
  WiFiClient c;
  bool ok = c.connect("1.1.1.1", 53, 4000);
  c.stop();
  return ok;
}

// ---------- AT-Hilfen, Anbieter-Erkennung, Guthaben ----------
String atRaw(const String& cmd, uint32_t timeout = 3000) {
  while (SerialAT.available()) SerialAT.read();
  SerialAT.print("AT" + cmd + "\r\n");
  String r; uint32_t t = millis();
  while (millis() - t < timeout) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf("\r\nOK") >= 0 || r.indexOf("ERROR") >= 0) break;
    delay(5);
  }
  return r;
}

// Netz-Kennung (MCC+MNC) auslesen und Anbieter zuordnen
void detectProvider() {
  atRaw("+COPS=3,2");
  String r = atRaw("+COPS?");
  int q = r.indexOf('"');
  int q2 = r.indexOf('"', q + 1);
  g_plmn = (q >= 0 && q2 > q) ? r.substring(q + 1, q2) : "";
  if (g_plmn == "22801") g_provider = "Swisscom";
  else if (g_plmn == "22802") g_provider = "Sunrise";
  else if (g_plmn == "22803") g_provider = "Salt";
  else g_provider = g_operator;
  atRaw("+COPS=3,0");  // Namensformat zurueck
}

// Betrag in CHF aus Netz-Text ziehen ("CHF 12.50" oder "12.50 CHF")
String parseChf(const String& t) {
  String u = t; u.toUpperCase();
  int i = u.indexOf("CHF");
  if (i < 0) i = u.indexOf("FR.");
  if (i < 0) return "";
  auto isNum = [](char c) { return isDigit(c) || c == '.' || c == ','; };
  // erst hinter dem Waehrungskuerzel suchen
  int a = i + 3;
  while (a < (int)t.length() && (t[a] == ' ' || t[a] == '.' || t[a] == ':')) a++;
  String num;
  while (a < (int)t.length() && isNum(t[a])) num += t[a++];
  if (!num.length()) {  // sonst davor
    int b = i - 1;
    while (b >= 0 && t[b] == ' ') b--;
    while (b >= 0 && isNum(t[b])) num = String(t[b--]) + num;
  }
  num.replace(",", ".");
  while (num.endsWith(".")) num.remove(num.length() - 1);
  return num;
}

// USSD senden und Antwort abwarten (Swisscom Prepaid: *130#)
bool fetchBalance() {
  atRaw("+CSCS=\"GSM\"");
  String r = atRaw("+CUSD=1,\"*130#\",15", 4000);
  uint32_t t = millis();
  while (millis() - t < 25000) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    int c = r.indexOf("+CUSD:");
    if (c >= 0) {
      int q1 = r.indexOf('"', c);
      int q2 = (q1 >= 0) ? r.indexOf('"', q1 + 1) : -1;
      if (q2 > q1) {
        String txt = r.substring(q1 + 1, q2);
        txt.trim();
        g_balanceMsg = txt.substring(0, 160);
        g_balance = parseChf(txt);
        g_balanceAt = millis();
        return g_balance.length() > 0;
      }
    }
    delay(50);
  }
  g_balanceMsg = "Keine Antwort vom Netz";
  return false;
}

// Ersatzweg: SMS "STATUS" an 444 (Swisscom), Antwort-SMS auslesen
bool fetchBalanceSms() {
  atRaw("+CMGF=1");
  atRaw("+CSCS=\"GSM\"");
  atRaw("+CNMI=2,1,0,0,0");
  while (SerialAT.available()) SerialAT.read();
  SerialAT.print("AT+CMGS=\"444\"\r");
  String r; uint32_t t = millis();
  while (millis() - t < 5000 && r.indexOf('>') < 0) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    delay(5);
  }
  if (r.indexOf('>') < 0) return false;
  SerialAT.print("STATUS");
  SerialAT.write(0x1A);
  r = ""; t = millis();
  String idx;
  while (millis() - t < 90000) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    int c = r.indexOf("+CMTI:");
    if (c >= 0) {
      int comma = r.indexOf(',', c);
      int e = r.indexOf('\n', comma);
      if (comma > 0 && e > comma) { idx = r.substring(comma + 1, e); idx.trim(); break; }
    }
    if (r.indexOf("ERROR") >= 0) return false;
    delay(50);
  }
  if (!idx.length()) { g_balanceMsg = "Keine SMS-Antwort"; return false; }
  String m = atRaw("+CMGR=" + idx, 4000);
  atRaw("+CMGD=" + idx);
  int h = m.indexOf("+CMGR:");
  int nl = (h >= 0) ? m.indexOf('\n', h) : -1;
  String body = (nl >= 0) ? m.substring(nl + 1) : m;
  int ok = body.indexOf("\r\nOK");
  if (ok >= 0) body = body.substring(0, ok);
  body.trim();
  g_balanceMsg = body.substring(0, 160);
  g_balance = parseChf(body);
  if (g_balance.length()) g_balanceAt = millis();
  return g_balance.length() > 0;
}

// ---------- 4G ----------
bool connect4G() {
  if (MODEM_PWRKEY_PIN >= 0) {
    pinMode(MODEM_PWRKEY_PIN, OUTPUT);
    digitalWrite(MODEM_PWRKEY_PIN, HIGH); delay(1000);
    digitalWrite(MODEM_PWRKEY_PIN, LOW);  delay(3000);
  }
  SerialAT.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(500);
  g_modemOk = modem.init() || modem.restart();
  if (!g_modemOk) { Serial.println("Modem nicht erreichbar (Pins pruefen)"); return false; }

  // SIM-PIN nur aus dem internen Speicher, nie im Log
  if (modem.getSimStatus() == SIM_LOCKED) {
    String pin = prefs.getString("pin", "");
    if (!pin.length()) { Serial.println("SIM gesperrt, keine PIN gespeichert"); return false; }
    modem.simUnlock(pin.c_str());
  }
  if (!modem.waitForNetwork(NETWORK_TIMEOUT_MS)) { Serial.println("Kein Mobilfunknetz"); return false; }

  g_operator = modem.getOperator();
  detectProvider();
  g_signal = modem.getSignalQuality();

  String apns[8]; int n;
  apnCandidates(g_operator, apns, n);
  String user = prefs.getString("apn_user", "");
  String pass = prefs.getString("apn_pass", "");
  for (int i = 0; i < n; i++) {
    Serial.printf("APN-Versuch: %s\n", apns[i].c_str());
    if (modem.gprsConnect(apns[i].c_str(), user.c_str(), pass.c_str())) {
      g_apnUsed = apns[i];
      g_ip = modem.getLocalIP();
      if (g_provider == "Swisscom") g_balanceReq = true;  // Guthaben automatisch holen
      return true;
    }
  }
  return false;
}

// ---------- WLAN ----------
bool connectWiFi() {
  String ssid = prefs.getString("ssid", "");
  if (!ssid.length()) return false;
  WiFi.begin(ssid.c_str(), prefs.getString("wpass", "").c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_TIMEOUT_MS) delay(250);
  if (WiFi.status() != WL_CONNECTED) return false;
  g_ip = WiFi.localIP().toString();
  return internetReachable();
}

// ---------- Netzwerk-Task: 4G -> WLAN -> Hotspot ----------
void netTask(void*) {
  for (;;) {
    bool needCheck = g_reconnect || !g_online;
    if (!needCheck) {
      // Verbindung beobachten
      if (g_mode == "4g") {
        g_signal = modem.getSignalQuality();
        if (!modem.isGprsConnected()) g_online = false;
      } else if (g_mode == "wifi" && WiFi.status() != WL_CONNECTED) {
        g_online = false;
      }
    }
    if (g_online && g_mode == "4g" && g_provider == "Swisscom") {
      // beim Start, auf Anfrage und danach alle 30 Minuten
      if (g_balanceReq || (g_balanceAt && millis() - g_balanceAt > 1800000UL)) {
        g_balanceReq = false;
        if (!fetchBalance()) fetchBalanceSms();
        notifyStatus();
      }
    }
    if (needCheck || !g_online) {
      g_reconnect = false;
      g_online = false; g_mode = "start";
      if (connect4G()) { g_mode = "4g"; g_online = true; }
      else if (connectWiFi()) { g_mode = "wifi"; g_online = true; }
      else { g_mode = "hotspot"; g_ip = WiFi.softAPIP().toString(); }
      Serial.printf("Modus: %s\n", g_mode.c_str());
      notifyStatus();
    }
    vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
  }
}

// ---------- Web ----------
const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CYBER TV BOX</title><style>
body{background:#0b0f1a;color:#e6f1ff;font-family:system-ui;margin:0;padding:16px}
h1{color:#00e5ff;margin:0 0 12px}.c{background:#141b2d;border:1px solid #1f2c4a;border-radius:12px;padding:14px;margin:12px 0}
input,button{width:100%;padding:10px;margin:4px 0;border-radius:8px;border:1px solid #2a3b63;background:#0b0f1a;color:#e6f1ff;box-sizing:border-box}
button{background:#00b8d4;color:#001;font-weight:700}pre{white-space:pre-wrap;font-size:13px}
</style></head><body><h1>CYBER TV BOX</h1>
<div class="c"><b>Status</b><pre id="st">...</pre></div>
<div class="c"><b>Guthaben (Swisscom Prepaid)</b><pre id="bal">...</pre><button onclick="p('/api/balance',{})">Guthaben jetzt abfragen</button></div>
<div class="c"><b>WLAN (Heimnetz)</b><input id="ssid" placeholder="SSID"><input id="wp" type="password" placeholder="Passwort">
<button onclick="p('/api/wifi',{ssid:ssid.value,pass:wp.value})">Speichern</button></div>
<div class="c"><b>Mobilfunk</b><input id="apn" placeholder="APN (leer = automatisch)"><input id="au" placeholder="APN-Benutzer"><input id="ap" type="password" placeholder="APN-Passwort">
<input id="pin" type="password" placeholder="SIM-PIN (optional)">
<button onclick="p('/api/mobile',{apn:apn.value,user:au.value,pass:ap.value,pin:pin.value})">Speichern</button></div>
<div class="c"><b>Hotspot-Passwort (min. 8 Zeichen)</b><input id="hp" type="password"><button onclick="p('/api/ap',{pass:hp.value})">Aendern</button></div>
<div class="c"><b>Fernseher 1-6</b><div id="tvs"></div><button onclick="saveTv()">TVs speichern</button></div>
<button onclick="p('/api/reconnect',{})">Verbindung neu aufbauen</button>
<button onclick="if(confirm('Neustart?'))p('/api/reboot',{})">Neustart</button>
<script>
async function p(u,b){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});alert(r.ok?'OK':'Fehler')}
async function st(){try{const j=await(await fetch('/api/status')).json();document.getElementById('st').textContent=JSON.stringify(j,null,1);document.getElementById('bal').textContent=(j.provider||'?')+(j.balance_chf?'\nGuthaben: CHF '+j.balance_chf:'\n'+(j.balance_msg||'noch keine Abfrage'))}catch(e){}}
let tv=[];async function lt(){tv=await(await fetch('/api/tvs')).json();const d=document.getElementById('tvs');d.innerHTML='';
for(let i=0;i<6;i++){const t=tv[i]||{name:'',room:'',addr:''};d.innerHTML+=`<b>TV ${i+1}</b><input id="n${i}" placeholder="Name" value="${t.name}"><input id="r${i}" placeholder="Raum/Benutzer" value="${t.room}"><input id="a${i}" placeholder="Adresse (IP)" value="${t.addr}">`}}
function saveTv(){const a=[];for(let i=0;i<6;i++)a.push({name:document.getElementById('n'+i).value,room:document.getElementById('r'+i).value,addr:document.getElementById('a'+i).value});p('/api/tvs',a)}
st();lt();setInterval(st,5000);
</script></body></html>)HTML";

bool bodyJson(JsonDocument& d) {
  return server.hasArg("plain") && !deserializeJson(d, server.arg("plain"));
}
void ok() { server.send(200, "application/json", "{\"ok\":true}"); }
void bad() { server.send(400, "application/json", "{\"ok\":false}"); }

void setupWeb() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/api/status", HTTP_GET, [] { server.send(200, "application/json", statusJson()); });

  server.on("/api/wifi", HTTP_POST, [] {
    JsonDocument d; if (!bodyJson(d)) return bad();
    prefs.putString("ssid", d["ssid"] | "");
    prefs.putString("wpass", d["pass"] | "");
    g_reconnect = true; ok();
  });
  server.on("/api/mobile", HTTP_POST, [] {
    JsonDocument d; if (!bodyJson(d)) return bad();
    prefs.putString("apn", d["apn"] | "");
    prefs.putString("apn_user", d["user"] | "");
    prefs.putString("apn_pass", d["pass"] | "");
    String pin = d["pin"] | "";
    if (pin.length()) prefs.putString("pin", pin);
    g_reconnect = true; ok();
  });
  server.on("/api/ap", HTTP_POST, [] {
    JsonDocument d; if (!bodyJson(d)) return bad();
    String p = d["pass"] | "";
    if (p.length() < 8) return bad();
    prefs.putString("appass", p);
    ok(); delay(500); ESP.restart();
  });
  server.on("/api/tvs", HTTP_GET, [] {
    server.send(200, "application/json", prefs.getString("tvs", "[]"));
  });
  server.on("/api/tvs", HTTP_POST, [] {
    JsonDocument d; if (!bodyJson(d) || !d.is<JsonArray>() || d.size() > MAX_TVS) return bad();
    String s; serializeJson(d, s); prefs.putString("tvs", s); ok();
  });
  server.on("/api/balance", HTTP_POST, [] { g_balanceReq = true; ok(); });
  server.on("/api/reconnect", HTTP_POST, [] { g_reconnect = true; ok(); });
  server.on("/api/reboot", HTTP_POST, [] { ok(); delay(300); ESP.restart(); });
  server.begin();
}

// ---------- BLE ----------
class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    String cmd = String(v.c_str());
    cmd.trim();
    g_lastCmd = cmd;
    Serial.printf("BLE-Befehl: %s\n", cmd.c_str());
    // Befehle: STATUS | RECONNECT | TV:<1-6>:<BEFEHL>  (TV-Befehle werden hier
    // nur angenommen/protokolliert; die Weitergabe an das jeweilige TV-Geraet
    // folgt in der naechsten Ausbaustufe.)
    if (cmd == "RECONNECT") g_reconnect = true;
    if (cmd == "BALANCE") g_balanceReq = true;
    notifyStatus();
  }
};

void setupBle() {
  NimBLEDevice::init(BLE_NAME);
  NimBLEServer* srv = NimBLEDevice::createServer();
  NimBLEService* svc = srv->createService(BLE_SERVICE_UUID);
  NimBLECharacteristic* cmd = svc->createCharacteristic(BLE_CMD_UUID, NIMBLE_PROPERTY::WRITE);
  cmd->setCallbacks(new CmdCallbacks());
  bleStatus = svc->createCharacteristic(BLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->start();
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  prefs.begin("cyberbox", false);

  // Hotspot ist immer an, damit Display/Handy die Box erreichen koennen
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, prefs.getString("appass", AP_DEFAULT_PASS).c_str());
  Serial.printf("Hotspot %s, IP %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  setupWeb();
  setupBle();
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  server.handleClient();
  static uint32_t t = 0;
  if (millis() - t > 5000) { t = millis(); notifyStatus(); }
  delay(2);
}
