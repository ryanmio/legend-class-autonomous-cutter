// telemetry.cpp
// WiFi AP + WebSocket + HTTP server for real-time telemetry and command handling.
//
// WebSocket broadcasts a JSON telemetry payload at 10 Hz.
// HTTP endpoints handle app commands (e-stop, mode switch, weapon control, etc.).
//
// Libraries: WiFi.h, WebServer.h, WebSocketsServer.h, ArduinoOTA.h, ArduinoJson.h

#include "telemetry.h"
#include "config.h"
#include "secrets.h"
#include "battery.h"
#include "bilge.h"
#include "gps.h"
#include "imu.h"
#include "sonar.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

static WebServer       httpServer(HTTP_PORT);
static WebSocketsServer wsServer(TELEMETRY_WS_PORT);
static unsigned long   lastBroadcastMs = 0;

// ---- WebSocket event handler ----
static void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_TEXT) {
    // TODO: parse incoming JSON commands from app (e-stop, mode, weapon control)
    Serial.printf("[WS] client %u: %s\n", num, payload);
  }
}

void telemetryBegin() {
  // Start WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CONN);
  Serial.print("[WIFI] AP started: ");
  Serial.println(WiFi.softAPIP());

  // OTA
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  // WebSocket
  wsServer.begin();
  wsServer.onEvent(onWsEvent);

  // HTTP routes
  telemetryRegisterRoutes();
  httpServer.begin();

  Serial.println("[TELEMETRY] Server ready");
}

void telemetryUpdate() {
  ArduinoOTA.handle();
  httpServer.handleClient();
  wsServer.loop();

  unsigned long now = millis();
  if (now - lastBroadcastMs >= TELEMETRY_INTERVAL_MS) {
    lastBroadcastMs = now;
    telemetryBroadcast();
  }
}

void telemetryBroadcast() {
  StaticJsonDocument<512> doc;
  const BatteryData& batt   = batteryGet();
  const BilgeData&   bilge  = bilgeGet();
  const GpsData&     gps    = gpsGet();
  const ImuData&     imu    = imuGet();
  const SonarData&   sonar  = sonarGet();

  doc["v"]          = FIRMWARE_VERSION;
  doc["uptime"]     = millis() / 1000;
  doc["heap"]       = ESP.getFreeHeap();
  doc["batt_v"]     = serialized(String(batt.voltageV, 2));
  doc["batt_a"]     = serialized(String(batt.currentA, 2));
  doc["batt_low"]   = batt.lowVoltage;
  doc["batt_crit"]  = batt.criticalVoltage;
  doc["bilge_fwd"]  = bilge.sensorFwd;
  doc["bilge_aft"]  = bilge.sensorAft;
  doc["pump"]       = bilge.pumpRunning;
  doc["gps_fix"]    = gps.fix;
  doc["lat"]        = serialized(String(gps.lat, 7));
  doc["lon"]        = serialized(String(gps.lon, 7));
  doc["speed_kts"]  = serialized(String(gps.speedKnots, 1));
  doc["course"]     = serialized(String(gps.courseTrue, 1));
  doc["sats"]       = gps.satellites;
  doc["heading"]    = serialized(String(imu.heading, 1));
  doc["roll"]       = serialized(String(imu.roll, 1));
  doc["pitch"]      = serialized(String(imu.pitch, 1));
  doc["depth_m"]    = serialized(String(sonar.depthM, 2));
  doc["sonar_ok"]   = sonar.valid;

  char buf[512];
  serializeJson(doc, buf);
  wsServer.broadcastTXT(buf);
}

void telemetryRegisterRoutes() {
  // Status / health check
  httpServer.on("/status", HTTP_GET, []() {
    StaticJsonDocument<128> doc;
    doc["ok"] = true;
    doc["v"]  = FIRMWARE_VERSION;
    doc["ip"] = WiFi.softAPIP().toString();
    char buf[128];
    serializeJson(doc, buf);
    httpServer.send(200, "application/json", buf);
  });

  // Emergency stop
  httpServer.on("/estop", HTTP_POST, []() {
    // TODO: set global e-stop flag → motorsKill() in main loop
    httpServer.send(200, "application/json", "{\"estop\":true}");
  });

  // TODO: /mode, /gun, /ciws, /radar, /audio, /anchor, /door, /pid endpoints
}
