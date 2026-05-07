/*
 * test_20_wifi_leds.ino
 *
 * Integration test: WiFi (STA → hotspot → AP fallback) + WebSocket telemetry
 * + LED HTTP control.
 *
 * WiFi connection priority:
 *   1. Home WiFi        (SECRET_HOME_SSID)     — bench testing
 *   2. iPhone hotspot   (SECRET_HOTSPOT_SSID)  — on the water
 *   3. AP fallback      "LegendCutter"          — last resort
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
 *   2. Install via Arduino Library Manager:
 *        "AsyncTCP"          by dvarrel
 *        "ESPAsyncWebServer" by lacamera
 *        "ArduinoJson"       by Benoit Blanchon
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "secrets.h"

// ── Config ────────────────────────────────────────────────────────────────────
static const uint8_t PIN_NAV    = 18;
static const uint8_t PIN_BRIDGE = 19;
static const uint8_t PIN_DECK   = 23;

static const unsigned long TX_INTERVAL_MS = 100;  // 10 Hz telemetry

// ── State ─────────────────────────────────────────────────────────────────────
static bool   navOn    = false;
static bool   bridgeOn = false;
static bool   deckOn   = false;
static String boatIP;

// HTTP on port 80, WebSocket on port 81 — matches app constants
static AsyncWebServer  httpServer(80);
static AsyncWebServer  wsApp(81);
static AsyncWebSocket  ws("/");
static unsigned long   lastTx = 0;

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
static void addCORS(AsyncWebServerResponse* r) {
  r->addHeader("Access-Control-Allow-Origin",  "*");
  r->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  r->addHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void applyLed(uint8_t pin, bool& state, bool on) {
  state = on;
  digitalWrite(pin, on ? HIGH : LOW);
  Serial.printf("[LED] GPIO%d -> %s\n", pin, on ? "ON" : "OFF");
}

static void broadcastTelemetry() {
  if (ws.count() == 0) return;

  StaticJsonDocument<192> doc;
  doc["v"]         = "test_20";
  doc["uptime"]    = millis() / 1000;
  doc["heap"]      = ESP.getFreeHeap();
  doc["nav_on"]    = navOn;
  doc["bridge_on"] = bridgeOn;
  doc["deck_on"]   = deckOn;

  char buf[192];
  serializeJson(doc, buf);
  ws.textAll(buf);
}

// ── HTTP handlers ─────────────────────────────────────────────────────────────
static void setupHTTP() {
  // CORS preflight for all endpoints
  httpServer.onNotFound([](AsyncWebServerRequest* req) {
    if (req->method() == HTTP_OPTIONS) {
      AsyncWebServerResponse* r = req->beginResponse(204);
      addCORS(r);
      req->send(r);
    } else {
      req->send(404);
    }
  });

  httpServer.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    String body = "{\"ok\":true,\"v\":\"test_20\",\"ip\":\"" + boatIP + "\"}";
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", body);
    addCORS(r);
    req->send(r);
  });

  // POST /led  body: {"light":"nav"|"bridge"|"deck", "state":true|false}
  httpServer.on(
    "/led", HTTP_POST,
    [](AsyncWebServerRequest* req) {},   // onRequest — body arrives below
    nullptr,                              // onUpload
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
       size_t /*index*/, size_t /*total*/) {

      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, data, len)) {
        req->send(400, "text/plain", "Bad JSON");
        return;
      }

      const char* light = doc["light"] | "";
      bool        on    = doc["state"]  | false;

      if      (strcmp(light, "nav")    == 0) applyLed(PIN_NAV,    navOn,    on);
      else if (strcmp(light, "bridge") == 0) applyLed(PIN_BRIDGE, bridgeOn, on);
      else if (strcmp(light, "deck")   == 0) applyLed(PIN_DECK,   deckOn,   on);
      else { req->send(400, "text/plain", "Unknown light — use nav|bridge|deck"); return; }

      AsyncWebServerResponse* r = req->beginResponse(200, "application/json", "{\"ok\":true}");
      addCORS(r);
      req->send(r);
    }
  );

  httpServer.begin();
  Serial.println("[HTTP] port 80  /status  /led");
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

  ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* client,
                AwsEventType type, void*, uint8_t*, size_t) {
    if (type == WS_EVT_CONNECT) {
      Serial.printf("[WS] Client %u connected\n", client->id());
      Serial.println("[GATE 2] PASS candidate — check Telemetry screen");
    } else if (type == WS_EVT_DISCONNECT) {
      Serial.printf("[WS] Client %u disconnected\n", client->id());
    }
  });
  wsApp.addHandler(&ws);
  wsApp.begin();
  Serial.println("[WS]  port 81");

  setupHTTP();

  Serial.println();
  Serial.printf("Ready — open app and tap SCAN  (boat IP: %s)\n", boatIP.c_str());
  Serial.println("========================================");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  if (millis() - lastTx >= TX_INTERVAL_MS) {
    lastTx = millis();
    broadcastTelemetry();
  }
}
