/*
 * test_20_wifi_leds.ino
 *
 * Integration test: WiFi AP + WebSocket telemetry + LED HTTP control.
 * This is the first WiFi/networking test for the boat.
 *
 * PASS criteria:
 *   GATE 1 — Serial prints "AP ready" and SSID is visible on phone
 *   GATE 2 — App connects to ws://192.168.4.1:81 (uptime increments on Telemetry screen)
 *   GATE 3 — Toggle NAV lights from app → GPIO 18 lights up
 *   GATE 4 — Toggle BRIDGE lights from app → GPIO 19 lights up
 *   GATE 5 — Toggle DECK lights from app → GPIO 23 lights up
 *
 * Hardware:
 *   GPIO 18 → Nav lights  (port red + starboard green)
 *   GPIO 19 → Bridge/interior lights
 *   GPIO 23 → Deck/flood lights
 *
 * Libraries (install via Arduino Library Manager before compiling):
 *   "WebSockets" by Markus Sattler     (arduinoWebSockets)
 *   "ArduinoJson" by Benoit Blanchon   (v6 or v7)
 *
 * Built-in (ESP32 Arduino core — no install needed):
 *   WiFi.h, WebServer.h
 *
 * Phone app: connect to SSID "LegendCutter" / "coastguard", then open app
 * and enter 192.168.4.1 as the boat IP. Navigate to Systems screen to toggle
 * LEDs; navigate to Telemetry to see uptime/heap confirming the WS stream.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

// ── Config ────────────────────────────────────────────────────────────────────
static const char* WIFI_SSID     = "LegendCutter";
static const char* WIFI_PASSWORD = "coastguard";

static const uint8_t PIN_NAV    = 18;
static const uint8_t PIN_BRIDGE = 19;
static const uint8_t PIN_DECK   = 23;

static const unsigned long TX_INTERVAL_MS = 100;  // 10 Hz

// ── State ─────────────────────────────────────────────────────────────────────
static bool navOn    = false;
static bool bridgeOn = false;
static bool deckOn   = false;

static WebServer        httpServer(80);
static WebSocketsServer wsServer(81);
static unsigned long    lastTx = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────
static void applyLed(uint8_t pin, bool& state, bool on) {
  state = on;
  digitalWrite(pin, on ? HIGH : LOW);
  Serial.printf("[LED] GPIO%d -> %s\n", pin, on ? "ON" : "OFF");
}

static void broadcastTelemetry() {
  StaticJsonDocument<192> doc;
  doc["v"]         = "test_20";
  doc["uptime"]    = millis() / 1000;
  doc["heap"]      = ESP.getFreeHeap();
  doc["nav_on"]    = navOn;
  doc["bridge_on"] = bridgeOn;
  doc["deck_on"]   = deckOn;

  char buf[192];
  serializeJson(doc, buf);
  wsServer.broadcastTXT(buf);
}

// ── HTTP handlers ─────────────────────────────────────────────────────────────
static void sendCORS() {
  httpServer.sendHeader("Access-Control-Allow-Origin",  "*");
  httpServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void handleStatus() {
  sendCORS();
  httpServer.send(200, "application/json",
    "{\"ok\":true,\"v\":\"test_20\",\"ip\":\"192.168.4.1\"}");
}

static void handleLed() {
  sendCORS();

  if (httpServer.method() == HTTP_OPTIONS) {
    httpServer.send(204);
    return;
  }
  if (httpServer.method() != HTTP_POST) {
    httpServer.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, httpServer.arg("plain"))) {
    httpServer.send(400, "text/plain", "Bad JSON");
    return;
  }

  const char* light = doc["light"] | "";
  bool        on    = doc["state"]  | false;

  if      (strcmp(light, "nav")    == 0) { applyLed(PIN_NAV,    navOn,    on); }
  else if (strcmp(light, "bridge") == 0) { applyLed(PIN_BRIDGE, bridgeOn, on); }
  else if (strcmp(light, "deck")   == 0) { applyLed(PIN_DECK,   deckOn,   on); }
  else {
    httpServer.send(400, "text/plain", "Unknown light — use nav|bridge|deck");
    return;
  }

  httpServer.send(200, "application/json", "{\"ok\":true}");
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

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] AP ready  SSID: %-20s  IP: %s\n",
                WIFI_SSID, WiFi.softAPIP().toString().c_str());
  Serial.println("[GATE 1] PASS candidate — connect phone to WiFi and verify SSID visible");

  httpServer.on("/status", HTTP_GET,     handleStatus);
  httpServer.on("/status", HTTP_OPTIONS, []{ sendCORS(); httpServer.send(204); });
  httpServer.on("/led",    HTTP_POST,    handleLed);
  httpServer.on("/led",    HTTP_OPTIONS, handleLed);
  httpServer.begin();
  Serial.println("[HTTP] port 80  endpoints: /status  /led");

  wsServer.begin();
  wsServer.onEvent([](uint8_t num, WStype_t type, uint8_t* /*payload*/, size_t /*len*/) {
    if (type == WStype_CONNECTED) {
      Serial.printf("[WS] Client %u connected\n", num);
      Serial.println("[GATE 2] PASS candidate — app received telemetry; check Telemetry screen");
    } else if (type == WStype_DISCONNECTED) {
      Serial.printf("[WS] Client %u disconnected\n", num);
    }
  });
  Serial.println("[WS]  port 81");

  Serial.println();
  Serial.println("Waiting for phone connection...");
  Serial.println("========================================");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  httpServer.handleClient();
  wsServer.loop();

  if (millis() - lastTx >= TX_INTERVAL_MS) {
    lastTx = millis();
    broadcastTelemetry();
  }
}
