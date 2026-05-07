/*
 * test_20_wifi_leds.ino
 *
 * Integration test: WiFi (STA → hotspot → AP fallback) + HTTP telemetry polling
 * + LED HTTP control.
 *
 * No external WebSocket library needed — only ArduinoJson + built-in WebServer.h.
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
 *   GATE 4 — Toggle BRIDGE lights from app → GPIO 19 lights up
 *   GATE 5 — Toggle DECK lights from app → GPIO 23 lights up
 *
 * Hardware:
 *   GPIO 18 → Nav lights  (port red + starboard green)
 *   GPIO 19 → Bridge/interior lights
 *   GPIO 23 → Deck/flood lights
 *
 * Setup:
 *   1. Copy secrets.h.example → secrets.h and fill in your WiFi credentials.
 *   2. Install via Arduino Library Manager (only one needed):
 *        "ArduinoJson" by Benoit Blanchon
 *   Built-in (no install): WiFi.h, WebServer.h
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "secrets.h"

// ── Config ────────────────────────────────────────────────────────────────────
static const uint8_t PIN_NAV    = 18;
static const uint8_t PIN_BRIDGE = 19;
static const uint8_t PIN_DECK   = 23;

// ── State ─────────────────────────────────────────────────────────────────────
static bool   navOn    = false;
static bool   bridgeOn = false;
static bool   deckOn   = false;
static String boatIP;

static WebServer server(80);

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
  String body = "{\"ok\":true,\"v\":\"test_20\",\"ip\":\"" + boatIP + "\"}";
  server.send(200, "application/json", body);
}

static void handleTelemetry() {
  addCORS();
  StaticJsonDocument<192> doc;
  doc["v"]         = "test_20";
  doc["uptime"]    = millis() / 1000;
  doc["heap"]      = ESP.getFreeHeap();
  doc["nav_on"]    = navOn;
  doc["bridge_on"] = bridgeOn;
  doc["deck_on"]   = deckOn;

  char buf[192];
  serializeJson(doc, buf);
  server.send(200, "application/json", buf);
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
  Serial.println("  test_20_wifi_leds");
  Serial.println("========================================");

  pinMode(PIN_NAV,    OUTPUT); digitalWrite(PIN_NAV,    LOW);
  pinMode(PIN_BRIDGE, OUTPUT); digitalWrite(PIN_BRIDGE, LOW);
  pinMode(PIN_DECK,   OUTPUT); digitalWrite(PIN_DECK,   LOW);
  Serial.println("[LED] GPIO 18/19/23 initialised LOW");

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
  server.handleClient();
}
