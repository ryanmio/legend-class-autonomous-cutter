/*
 * test_21_wifi_gps.ino
 *
 * Integration test: WiFi (STA → hotspot → AP fallback) + HTTP telemetry polling
 * + LED HTTP control + BN-220 GPS via TinyGPSPlus.
 *
 * Extends test_20 by reading the BN-220 GPS module and including position data
 * in the /telemetry JSON response.
 *
 * GPS wiring (this boat's harness — wire colours reversed from convention):
 *   ESP32 GPIO 17 (RX) → BN-220 WHITE wire (GPS TX)
 *   ESP32 GPIO  4 (TX) → BN-220 GREEN  wire (GPS RX) — unused, no config sent
 *   Baud: 9600
 *
 * WiFi connection priority:
 *   1. Home WiFi        (SECRET_HOME_SSID)    — bench testing
 *   2. iPhone hotspot   (SECRET_HOTSPOT_SSID) — on the water
 *   3. AP fallback      "LegendCutter"         — last resort
 *
 * PASS criteria:
 *   GATE 1 — Serial shows "Connected" + IP address
 *   GATE 2 — App SCAN finds boat; Telemetry screen shows uptime incrementing
 *   GATE 3 — Toggle NAV lights from app → GPIO 18 lights up
 *   GATE 4 — Serial shows NMEA bytes arriving from GPS
 *   GATE 5 — /telemetry returns "gps_fix":true with real coordinates
 *
 * Hardware:
 *   GPIO 17 → GPS RX  (BN-220 TX, white wire)
 *   GPIO  4 → GPS TX  (BN-220 RX, green wire — not used)
 *   GPIO 18 → Nav lights  (port red + starboard green)
 *   GPIO 19 → Bridge/interior lights
 *   GPIO 23 → Deck/flood lights
 *
 * Libraries (Arduino Library Manager):
 *   "ArduinoJson"   by Benoit Blanchon
 *   "TinyGPSPlus"   by Mikal Hart
 * Built-in (no install): WiFi.h, WebServer.h
 *
 * Setup:
 *   1. Copy secrets.h.example → secrets.h and fill in your WiFi credentials.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>
#include "secrets.h"

// ── Config ────────────────────────────────────────────────────────────────────
static const uint8_t  PIN_NAV    = 18;
static const uint8_t  PIN_BRIDGE = 19;
static const uint8_t  PIN_DECK   = 23;
static const uint8_t  GPS_RX_PIN = 17;   // white wire — GPS TX
static const uint8_t  GPS_TX_PIN = 4;    // green wire — GPS RX (unused)
static const uint32_t GPS_BAUD   = 9600;

// ── State ─────────────────────────────────────────────────────────────────────
static bool   navOn    = false;
static bool   bridgeOn = false;
static bool   deckOn   = false;
static String boatIP;

static WebServer      server(80);
static TinyGPSPlus    gps;
static HardwareSerial gpsSerial(2);

// ── WiFi ──────────────────────────────────────────────────────────────────────
static bool tryConnect(const char* ssid, const char* pass, int timeoutSecs) {
  WiFi.begin(ssid, pass);
  Serial.printf("[WiFi] Trying %-28s", ssid);
  for (int i = 0; i < timeoutSecs * 2; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      boatIP = WiFi.localIP().toString();
      Serial.printf("  OK  %s\n", boatIP.c_str());
      return true;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("  FAIL");
  WiFi.disconnect(true);
  delay(100);
  return false;
}

static void wifiSetup() {
  WiFi.mode(WIFI_STA);
  if (tryConnect(SECRET_HOME_SSID,    SECRET_HOME_PASS,    10)) return;
  if (tryConnect(SECRET_HOTSPOT_SSID, SECRET_HOTSPOT_PASS, 10)) return;

  Serial.println("[WiFi] No known network — starting AP fallback");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("LegendCutter", "coastguard");
  boatIP = WiFi.softAPIP().toString();
  Serial.printf("[WiFi] AP  SSID: LegendCutter  IP: %s\n", boatIP.c_str());
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void applyLed(uint8_t pin, bool& state, bool on) {
  state = on;
  digitalWrite(pin, on ? HIGH : LOW);
  Serial.printf("[LED] GPIO%d -> %s\n", pin, on ? "ON" : "OFF");
}

// ── HTTP handlers ─────────────────────────────────────────────────────────────
static void handleOptions() {
  addCORS();
  server.send(204);
}

static void handleStatus() {
  addCORS();
  String body = "{\"ok\":true,\"v\":\"test_21\",\"ip\":\"" + boatIP + "\"}";
  server.send(200, "application/json", body);
}

static void handleTelemetry() {
  addCORS();
  StaticJsonDocument<384> doc;
  doc["v"]         = "test_21";
  doc["uptime"]    = millis() / 1000;
  doc["heap"]      = ESP.getFreeHeap();
  doc["nav_on"]    = navOn;
  doc["bridge_on"] = bridgeOn;
  doc["deck_on"]   = deckOn;

  bool hasFix = gps.location.isValid() && gps.location.age() < 5000;
  doc["gps_fix"] = hasFix;
  doc["sats"]    = (int)gps.satellites.value();

  if (hasFix) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.6f", gps.location.lat());
    doc["lat"] = buf;
    snprintf(buf, sizeof(buf), "%.6f", gps.location.lng());
    doc["lon"] = buf;
    snprintf(buf, sizeof(buf), "%.1f", gps.speed.knots());
    doc["speed_kts"] = buf;
    snprintf(buf, sizeof(buf), "%.1f", gps.course.deg());
    doc["course"] = buf;
  }

  char out[384];
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleLed() {
  addCORS();
  if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
  if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Bad JSON");
    return;
  }

  const char* light = doc["light"] | "";
  bool        on    = doc["state"]  | false;

  if      (strcmp(light, "nav")    == 0) applyLed(PIN_NAV,    navOn,    on);
  else if (strcmp(light, "bridge") == 0) applyLed(PIN_BRIDGE, bridgeOn, on);
  else if (strcmp(light, "deck")   == 0) applyLed(PIN_DECK,   deckOn,   on);
  else { server.send(400, "text/plain", "Unknown light — use nav|bridge|deck"); return; }

  server.send(200, "application/json", "{\"ok\":true}");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_21_wifi_gps");
  Serial.println("========================================");

  pinMode(PIN_NAV,    OUTPUT); digitalWrite(PIN_NAV,    LOW);
  pinMode(PIN_BRIDGE, OUTPUT); digitalWrite(PIN_BRIDGE, LOW);
  pinMode(PIN_DECK,   OUTPUT); digitalWrite(PIN_DECK,   LOW);
  Serial.println("[LED] GPIO 18/19/23 initialised LOW");

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.printf("[GPS] HardwareSerial(2) RX=GPIO%d TX=GPIO%d @ %d baud\n",
                GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

  wifiSetup();
  Serial.println("[GATE 1] PASS candidate — see IP above");

  server.on("/status",    HTTP_GET,     handleStatus);
  server.on("/status",    HTTP_OPTIONS, handleOptions);
  server.on("/telemetry", HTTP_GET,     handleTelemetry);
  server.on("/telemetry", HTTP_OPTIONS, handleOptions);
  server.on("/led",       HTTP_POST,    handleLed);
  server.on("/led",       HTTP_OPTIONS, handleOptions);
  server.begin();

  Serial.printf("[HTTP] port 80  /status  /telemetry  /led  (IP: %s)\n", boatIP.c_str());
  Serial.println();
  Serial.println("Ready — open app and tap SCAN");
  Serial.println("========================================");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  while (gpsSerial.available()) {
    uint8_t c = gpsSerial.read();
    gps.encode(c);
    Serial.write(c);   // [GATE 4] mirror raw NMEA to Serial so we can verify bytes arriving
  }
  server.handleClient();
}
