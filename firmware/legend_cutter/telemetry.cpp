// telemetry.cpp
// WiFi (home first, hotspot second, no AP fallback) + HTTP server with the
// full /telemetry JSON and all command endpoints. Mirrors test_29 1:1.

#include "telemetry.h"
#include "config.h"
#include "secrets.h"
#include "ibus.h"
#include "motors.h"
#include "imu.h"
#include "gps.h"
#include "navigation.h"
#include "battery.h"
#include "bilge.h"
#include "sonar.h"
#include "audio.h"
#include "radar.h"
#include "lights.h"
#include "weapons.h"

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_system.h>

// Provided by legend_cutter.ino — current vessel mode label.
extern const char* vesselModeName();

static WebServer server(HTTP_PORT);
static String    boatIP;
static uint16_t  cruiseUs = DEFAULT_CRUISE_US;
static uint32_t  sessionId = 0;     // hardware-random, set once in telemetryBegin()

uint16_t    telemetryCruiseUs() { return cruiseUs; }
const char* telemetryBoatIP()   { return boatIP.c_str(); }

// ── WiFi ───────────────────────────────────────────────────────────────────
static bool tryConnect(const char* ssid, const char* pass, int timeoutSecs) {
    WiFi.begin(ssid, pass);
    Serial.printf("[WiFi] Trying %s", ssid);
    for (int i = 0; i < timeoutSecs * 2; i++) {
        if (WiFi.status() == WL_CONNECTED) {
            boatIP = WiFi.localIP().toString();
            Serial.printf(" OK  %s\n", boatIP.c_str());
            return true;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println(" FAIL");
    WiFi.disconnect(true);
    delay(100);
    return false;
}

// ── CORS ───────────────────────────────────────────────────────────────────
static void addCORS() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}
static void handleOptions() { addCORS(); server.send(204); }

// ── Handlers ───────────────────────────────────────────────────────────────
static void handleStatus() {
    addCORS();
    server.send(200, "application/json",
        String("{\"ok\":true,\"v\":\"") + FIRMWARE_VERSION +
        "\",\"ip\":\"" + boatIP + "\"}");
}

static void handleTelemetry() {
    addCORS();
    StaticJsonDocument<1536> doc;
    doc["v"]            = FIRMWARE_VERSION;
    doc["session_id"]   = sessionId;
    doc["uptime"]       = millis() / 1000;
    doc["heap"]         = ESP.getFreeHeap();
    doc["mode"]         = vesselModeName();
    doc["cruise_us"]    = cruiseUs;
    doc["rc_ever_good"] = ibusEverGood();
    doc["rc_age_ms"]    = ibusEverGood() ? (millis() - ibusLastFrameMs()) : 0;
    doc["rudder_us"]    = motorsRudderUs();
    doc["esc_us"]       = motorsPortUs();
    doc["ch_throttle"]  = ibusChannel(IBUS_IDX_THROTTLE);
    doc["ch_rudder"]    = ibusChannel(IBUS_IDX_RUDDER);
    doc["ch_reverse"]   = ibusChannel(IBUS_IDX_REVERSE);
    doc["ch_mode"]      = ibusChannel(IBUS_IDX_MODE);
    doc["ch_guard"]     = ibusChannel(IBUS_IDX_FAILSAFE_GUARD);
    doc["ch_gun_pan"]   = ibusChannel(IBUS_IDX_GUN_PAN);
    doc["gun_pan_us"]   = weaponsGunPanUs();

    doc["nav_on"]       = lightsState(LED_NAV);
    doc["bridge_on"]    = lightsState(LED_BRIDGE);
    doc["deck_on"]      = lightsState(LED_DECK);

    doc["audio_ok"]     = audioAvailable();

    doc["bilge_fwd"]      = bilgeFwdWet();
    doc["bilge_mid"]      = bilgeMidWet();
    doc["bilge_rear"]     = bilgeRearWet();
    doc["pump"]           = bilgePumpOn();
    doc["pump_manual"]    = bilgePumpManual();
    doc["pump_stuck"]     = bilgeStuck();
    BilgePhase phase      = bilgePumpPhase();
    doc["pump_phase"]     = (phase == BILGE_PHASE_ON)    ? "on"
                          : (phase == BILGE_PHASE_PAUSE) ? "pause"
                          : "off";
    if (phase != BILGE_PHASE_OFF) {
        doc["pump_cycle"]    = bilgePumpCycle();
        doc["pump_phase_ms"] = bilgePumpPhaseMs();
    }

    doc["radar_on"]       = radarOn();
    doc["radar_speed"]    = radarSpeed();
    doc["radar_burst_ms"] = radarBurstMs();
    doc["radar_pause_ms"] = radarPauseMs();

    doc["depth_mode"] = (sonarMode() == DEPTH_RUN) ? "run" : "off";
    char buf[24];
    if (sonarLastDepthM() >= 0.0f) {
        snprintf(buf, sizeof(buf), "%.2f", sonarLastDepthM());
        doc["depth_m"] = buf;
    }
    if (sonarLastReadMs() > 0) {
        doc["depth_age_ms"] = millis() - sonarLastReadMs();
    }

    snprintf(buf, sizeof(buf), "%.1f", imuHeading()); doc["heading"] = buf;
    if (batteryAvailable()) {
        snprintf(buf, sizeof(buf), "%.2f", batteryVolts()); doc["batt_v"] = buf;
        snprintf(buf, sizeof(buf), "%.2f", batteryAmps());  doc["batt_a"] = buf;
    }

    doc["gps_fix"]       = gpsValid();
    doc["gps_simulated"] = gpsSimulated();
    if (gpsValid()) {
        snprintf(buf, sizeof(buf), "%.6f", gpsLat()); doc["lat"] = buf;
        snprintf(buf, sizeof(buf), "%.6f", gpsLon()); doc["lon"] = buf;
    }
    if (!gpsSimulated()) {
        doc["sats"] = (int)gpsSats();
        if (gpsSpeedValid())  { snprintf(buf, sizeof(buf), "%.1f", gpsSpeedKnots()); doc["speed_kts"] = buf; }
        if (gpsCourseValid()) { snprintf(buf, sizeof(buf), "%.1f", gpsCourseDeg());  doc["course"]    = buf; }
    }

    doc["wp_set"]      = navWpSet();
    doc["captured"]    = navCaptured();
    CapturedBy cb = navCapturedBy();
    doc["captured_by"] = (cb == CAPTURED_BY_DISTANCE) ? "distance"
                       : (cb == CAPTURED_BY_CROSSING) ? "crossing"
                       : "none";
    if (navWpSet()) {
        snprintf(buf, sizeof(buf), "%.6f", navWpLat()); doc["wp_lat"] = buf;
        snprintf(buf, sizeof(buf), "%.6f", navWpLon()); doc["wp_lon"] = buf;
        if (gpsValid()) {
            snprintf(buf, sizeof(buf), "%.1f", navWpDistM());   doc["wp_dist_m"]  = buf;
            snprintf(buf, sizeof(buf), "%.1f", navWpBearing()); doc["wp_bearing"] = buf;
        }
        if (navStartValid()) {
            snprintf(buf, sizeof(buf), "%.6f", navStartLat()); doc["wp_start_lat"] = buf;
            snprintf(buf, sizeof(buf), "%.6f", navStartLon()); doc["wp_start_lon"] = buf;
        }
    }

    snprintf(buf, sizeof(buf), "%.2f", pidKp()); doc["pid_kp"] = buf;
    snprintf(buf, sizeof(buf), "%.2f", pidKd()); doc["pid_kd"] = buf;

    char out[1536];
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleCruise() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    uint16_t newUs = 0;
    if (req.containsKey("us")) {
        int us = req["us"].as<int>();
        if (us < NEUTRAL_US || us > MAX_FWD_US) {
            server.send(400, "application/json",
                "{\"ok\":false,\"err\":\"us out of [1500..1800]\"}");
            return;
        }
        newUs = (uint16_t)us;
    } else if (req.containsKey("pct")) {
        float pct = req["pct"].as<float>();
        if (pct < 0.0f)   pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        newUs = (uint16_t)(NEUTRAL_US + (MAX_FWD_US - NEUTRAL_US) * (pct / 100.0f));
    } else {
        server.send(400, "text/plain", "Need 'us' or 'pct'");
        return;
    }

    cruiseUs = newUs;

    StaticJsonDocument<128> resp;
    resp["ok"]        = true;
    resp["cruise_us"] = cruiseUs;
    resp["cap"]       = AUTO_CRUISE_CAP_US;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleWaypoint() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    if (req["lat"].isNull() || req["lon"].isNull()) {
        navClearWaypoint();
        server.send(200, "application/json", "{\"ok\":true,\"cleared\":true}");
        return;
    }
    if (!req.containsKey("lat") || !req.containsKey("lon")) {
        server.send(400, "text/plain", "Need lat and lon");
        return;
    }
    float lat = req["lat"].as<float>();
    float lon = req["lon"].as<float>();
    navSetWaypoint(lat, lon);

    StaticJsonDocument<128> resp;
    resp["ok"]  = true;
    resp["lat"] = lat;
    resp["lon"] = lon;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handlePid() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    float kp = pidKp();
    float kd = pidKd();
    if (req.containsKey("kp")) {
        kp = req["kp"].as<float>();
        if (kp < 0.0f || kp > 20.0f) {
            server.send(400, "application/json",
                "{\"ok\":false,\"err\":\"kp out of [0..20]\"}");
            return;
        }
    }
    if (req.containsKey("kd")) {
        kd = req["kd"].as<float>();
        if (kd < 0.0f || kd > 30.0f) {
            server.send(400, "application/json",
                "{\"ok\":false,\"err\":\"kd out of [0..30]\"}");
            return;
        }
    }
    setPidGains(kp, kd);

    StaticJsonDocument<128> resp;
    resp["ok"] = true;
    resp["kp"] = pidKp();
    resp["kd"] = pidKd();
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleSimGps() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }
    if (!req.containsKey("lat") || !req.containsKey("lon")) {
        server.send(400, "text/plain", "Need lat and lon");
        return;
    }
    float lat = req["lat"].as<float>();
    float lon = req["lon"].as<float>();
    gpsSimSet(lat, lon);

    StaticJsonDocument<128> resp;
    resp["ok"]            = true;
    resp["lat"]           = lat;
    resp["lon"]           = lon;
    resp["gps_simulated"] = true;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleLed() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    const char* light = req["light"] | "";
    bool        on    = req["state"]  | false;
    LedId id;
    if      (!strcmp(light, "nav"))    id = LED_NAV;
    else if (!strcmp(light, "bridge")) id = LED_BRIDGE;
    else if (!strcmp(light, "deck"))   id = LED_DECK;
    else { server.send(400, "text/plain", "Unknown light"); return; }

    lightsSet(id, on);
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleAudio() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    if (!audioAvailable()) {
        server.send(503, "application/json",
            "{\"ok\":false,\"err\":\"DF1201S not initialised\"}");
        return;
    }

    StaticJsonDocument<96> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }
    const char* sound = req["sound"] | "";

    AudioClip clip;
    if      (!strcmp(sound, "horn"))  clip = AUDIO_HORN;
    else if (!strcmp(sound, "gun"))   clip = AUDIO_GUN;
    else if (!strcmp(sound, "board")) clip = AUDIO_BOARD;
    else {
        server.send(400, "application/json", "{\"ok\":false,\"err\":\"unknown sound\"}");
        return;
    }
    audioPlay(clip);

    StaticJsonDocument<96> resp;
    resp["ok"]    = true;
    resp["sound"] = sound;
    char out[96];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleBilge() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    bool on = req["on"] | false;
    bilgeSetManual(on);

    StaticJsonDocument<128> resp;
    resp["ok"]          = true;
    resp["pump_manual"] = bilgePumpManual();
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleRadar() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<192> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    // All fields optional — apply only those present.
    bool     on = radarOn();
    uint8_t  s  = radarSpeed();
    uint32_t b  = radarBurstMs();
    uint32_t p  = radarPauseMs();

    if (req.containsKey("on"))       on = req["on"].as<bool>();
    if (req.containsKey("speed"))    s  = (uint8_t)req["speed"].as<int>();
    if (req.containsKey("burst_ms")) b  = (uint32_t)req["burst_ms"].as<long>();
    if (req.containsKey("pause_ms")) p  = (uint32_t)req["pause_ms"].as<long>();
    radarSet(on, s, b, p);

    StaticJsonDocument<160> resp;
    resp["ok"]       = true;
    resp["on"]       = radarOn();
    resp["speed"]    = radarSpeed();
    resp["burst_ms"] = radarBurstMs();
    resp["pause_ms"] = radarPauseMs();
    char out[160];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleDepth() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<96> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    const char* m = req["mode"] | "";
    if      (!strcmp(m, "stop"))  sonarSetMode(DEPTH_OFF);
    else if (!strcmp(m, "check")) sonarPingNow();
    else if (!strcmp(m, "run"))   sonarSetMode(DEPTH_RUN);
    else {
        server.send(400, "application/json",
            "{\"ok\":false,\"err\":\"mode must be stop|check|run\"}");
        return;
    }

    StaticJsonDocument<128> resp;
    resp["ok"]   = true;
    resp["mode"] = (sonarMode() == DEPTH_RUN) ? "run" : "off";
    if (sonarLastDepthM() >= 0.0f) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%.2f", sonarLastDepthM());
        resp["depth_m"] = buf;
    }
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

void telemetryBegin() {
    sessionId = esp_random();   // app uses this to detect mid-flight reboots
    WiFi.mode(WIFI_STA);
    if (!tryConnect(SECRET_HOME_SSID, SECRET_HOME_PASS, 10)) {
        if (!tryConnect(SECRET_HOTSPOT_SSID, SECRET_HOTSPOT_PASS, 10)) {
            Serial.println("[WiFi] no network — operating without HTTP. RC + autopilot still active.");
        }
    }

    server.on("/status",    HTTP_GET,     handleStatus);
    server.on("/status",    HTTP_OPTIONS, handleOptions);
    server.on("/telemetry", HTTP_GET,     handleTelemetry);
    server.on("/telemetry", HTTP_OPTIONS, handleOptions);
    server.on("/cruise",    HTTP_POST,    handleCruise);
    server.on("/cruise",    HTTP_OPTIONS, handleOptions);
    server.on("/waypoint",  HTTP_POST,    handleWaypoint);
    server.on("/waypoint",  HTTP_OPTIONS, handleOptions);
    server.on("/pid",       HTTP_POST,    handlePid);
    server.on("/pid",       HTTP_OPTIONS, handleOptions);
    server.on("/sim_gps",   HTTP_POST,    handleSimGps);
    server.on("/sim_gps",   HTTP_OPTIONS, handleOptions);
    server.on("/led",       HTTP_POST,    handleLed);
    server.on("/led",       HTTP_OPTIONS, handleOptions);
    server.on("/audio",     HTTP_POST,    handleAudio);
    server.on("/audio",     HTTP_OPTIONS, handleOptions);
    server.on("/bilge",     HTTP_POST,    handleBilge);
    server.on("/bilge",     HTTP_OPTIONS, handleOptions);
    server.on("/radar",     HTTP_POST,    handleRadar);
    server.on("/radar",     HTTP_OPTIONS, handleOptions);
    server.on("/depth",     HTTP_POST,    handleDepth);
    server.on("/depth",     HTTP_OPTIONS, handleOptions);
    server.begin();
    if (boatIP.length()) {
        Serial.printf("HTTP up at http://%s/\n", boatIP.c_str());
    }
}

void telemetryUpdate() { server.handleClient(); }
