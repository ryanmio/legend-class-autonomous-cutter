// telemetry.cpp
// WiFi (scan-first: home → any "iPhone" hotspot → blind fallback; no AP) +
// non-blocking in-loop reconnect, plus the HTTP server with the full
// /telemetry JSON and all command endpoints. Wire contract mirrors the PASS'd
// test_29 build 1:1 (key set, key order, routes) — the phone app is firmware-
// agnostic only as long as that holds.

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
#include "histlog.h"
#include "cmd.h"

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_wifi.h>

// Provided by legend_cutter.ino — current vessel mode label + failsafe ack.
extern const char* vesselModeName();
extern bool        vesselFailsafeAck();

static WebServer    server(HTTP_PORT);
static void         networkTask(void*);    // core-0 task; owns handleClient()/wifiMaintain()
static TaskHandle_t netTaskHandle = NULL;  // for stack high-water queries from the loop console
static String       boatIP;
static uint16_t  cruiseUs  = DEFAULT_CRUISE_US;
static uint32_t  sessionId = 0;     // hardware-random, set once in telemetryBegin()

uint16_t    telemetryCruiseUs()             { return cruiseUs; }
void        telemetrySetCruiseUs(uint16_t us) { cruiseUs = us; }  // control loop only (cmdApply)
const char* telemetryBoatIP()               { return boatIP.c_str(); }

// Network task's worst-case-ever free stack (bytes). Queried from the loop's
// serial console; uxTaskGetStackHighWaterMark works cross-task given the handle.
uint32_t    telemetryNetStackFreeBytes() {
    return netTaskHandle ? (uint32_t)(uxTaskGetStackHighWaterMark(netTaskHandle) * sizeof(StackType_t)) : 0;
}

// ── WiFi (scan-first connect + non-blocking maintain, ported from test_29) ──
static String lastSsid;
static String lastPass;
static bool   wifiWasConnected = false;

// Boot-time nav-light signal so the operator can read WiFi state across the
// pool: 1 flash when scanning starts, 2 flashes once connected. Setup-only
// (boat neutral on the bench); leaves the LED off afterward.
static void blinkNav(int times) {
    for (int i = 0; i < times; i++) {
        digitalWrite(PIN_NAV, HIGH); delay(120);
        digitalWrite(PIN_NAV, LOW);  delay(120);
    }
}

// STA-link range/robustness tuning. Telemetry/command only — does NOT touch the
// RC/iBUS control path or the FSM. Split in two: the protocol/bandwidth pins must
// be set on the started driver before associating; the power-save + TX-power
// asserts are re-applied on every (re)connect because association can reset them.
//
// 11b is kept enabled so rate control retains the longest-range basic-rate
// fallback; 20 MHz is pinned for edge robustness. Channel is the AP's to choose
// (a STA follows it), so it's a router-side setting, not a firmware lever.
static void wifiSetStaRadioConfig() {
    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
}

static void wifiAssertLinkTuning() {
    WiFi.setSleep(false);            // no Wi-Fi modem sleep → steadier polls at range
    int8_t before = 0, after = 0;
    esp_wifi_get_max_tx_power(&before);
    esp_wifi_set_max_tx_power(84);   // driver max (unit = 0.25 dBm → ~20 dBm)
    esp_wifi_get_max_tx_power(&after);
    Serial.printf("[WiFi] TX power %.2f→%.2f dBm; modem-sleep off\n",
                  before * 0.25f, after * 0.25f);
}

static void wifiConnect() {
    Serial.println();
    Serial.println("[WiFi] Scanning…");
    blinkNav(1);
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    wifiSetStaRadioConfig();
    int n = WiFi.scanNetworks();
    Serial.printf("[WiFi] %d networks found\n", n);

    String ssid;
    String pass;
    bool foundHome = false;

    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        Serial.printf("  [%d] '%s' RSSI=%d\n", i, s.c_str(), WiFi.RSSI(i));
        if (s == SECRET_HOME_SSID) {
            ssid = s; pass = SECRET_HOME_PASS; foundHome = true;
            Serial.printf("[WiFi] matched home: %s\n", s.c_str());
            break;
        }
    }

    if (!foundHome) {
        for (int i = 0; i < n; i++) {
            String s = WiFi.SSID(i);
            if (s.indexOf("iPhone") >= 0 || s == SECRET_HOTSPOT_SSID) {
                ssid = s; pass = SECRET_HOTSPOT_PASS;
                Serial.printf("[WiFi] matched hotspot: %s\n", s.c_str());
                break;
            }
        }
    }

    if (ssid.length() == 0) {
        ssid = SECRET_HOTSPOT_SSID;
        pass = SECRET_HOTSPOT_PASS;
        Serial.printf("[WiFi] no scan match — blind-connecting: %s\n", ssid.c_str());
    }

    lastSsid = ssid;
    lastPass = pass;
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.print("[WiFi] connecting");
    for (int i = 0; i < 40; i++) {       // 20 s
        if (WiFi.status() == WL_CONNECTED) {
            boatIP = WiFi.localIP().toString();
            wifiWasConnected = true;
            wifiAssertLinkTuning();
            Serial.printf(" OK %s\n", boatIP.c_str());
            blinkNav(2);
            return;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.printf(" FAIL (status=%d)\n", WiFi.status());
}

// Non-blocking reconnect, called every loop. On WiFi loss it re-issues
// WiFi.begin() (returns immediately) at most once per 30 s using the boot
// credentials. No scan, no delay — outputs/iBUS/failsafe keep running.
static void wifiMaintain() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiWasConnected) {
            wifiWasConnected = true;
            boatIP = WiFi.localIP().toString();
            wifiAssertLinkTuning();
            Serial.printf("[WiFi] reconnected: %s\n", boatIP.c_str());
        }
        return;
    }
    if (wifiWasConnected) {
        wifiWasConnected = false;
        Serial.println("[WiFi] lost — retrying in background; boat unaffected");
    }
    static uint32_t lastAttemptMs = 0;
    if (millis() - lastAttemptMs >= 30000 && lastSsid.length() > 0) {
        lastAttemptMs = millis();
        WiFi.begin(lastSsid.c_str(), lastPass.c_str());
    }
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
    StaticJsonDocument<2304> doc;   // headroom for port_us/stbd_us (overflow drops fields silently)
    doc["v"]            = FIRMWARE_VERSION;
    doc["session_id"]   = sessionId;
    doc["uptime"]       = millis() / 1000;
    doc["heap"]         = ESP.getFreeHeap();
    doc["mode"]         = vesselModeName();
    doc["cruise_us"]    = cruiseUs;
    doc["failsafe_ack"] = vesselFailsafeAck();
    doc["rc_ever_good"] = ibusEverGood();
    doc["rc_age_ms"]    = ibusEverGood() ? (millis() - ibusLastFrameMs()) : 0;
    doc["rudder_us"]    = motorsRudderUs();
    doc["esc_us"]       = motorsPortUs();
    doc["port_us"]      = motorsPortUs();   // explicit port/stbd so AUTO diff-thrust split is readable
    doc["stbd_us"]      = motorsStbdUs();
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
    doc["bilge_fwd"]    = bilgeFwdWet();
    doc["bilge_mid"]    = bilgeMidWet();
    doc["bilge_rear"]   = bilgeRearWet();
    doc["pump"]         = bilgePumpOn();
    doc["pump_manual"]  = bilgePumpManual();
    doc["pump_stuck"]   = bilgeStuck();
    BilgePhase phase    = bilgePumpPhase();
    doc["pump_phase"]   = (phase == BILGE_PHASE_ON)    ? "on"
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
    if (sonarLastDepthM() >= 0.0f) {
        char dbuf[12];
        snprintf(dbuf, sizeof(dbuf), "%.2f", sonarLastDepthM());
        doc["depth_m"] = dbuf;
    }
    if (sonarLastReadMs() > 0) {
        doc["depth_age_ms"]  = millis() - sonarLastReadMs();
        doc["depth_raw_us"]  = sonarLastRawUs();  // diagnostic: 0=timeout, floor band=no bottom return
    }

    char buf[24];
    // `heading` is the boat's best TRUE heading (mag + declination + COG
    // trim) — directly comparable to `course` and `wp_bearing`.
    snprintf(buf, sizeof(buf), "%.1f", imuHeadingTrue()); doc["heading"]     = buf;
    snprintf(buf, sizeof(buf), "%.1f", imuHeadingMag());  doc["heading_mag"] = buf;
    snprintf(buf, sizeof(buf), "%.1f", imuCogTrim());     doc["cog_trim"]    = buf;
    if (batteryAvailable()) {
        snprintf(buf, sizeof(buf), "%.2f", batteryVolts()); doc["batt_v"] = buf;
        snprintf(buf, sizeof(buf), "%.2f", batteryAmps());  doc["batt_a"] = buf;
    }

    // Position
    doc["gps_fix"] = gpsValid();
    if (gpsValid()) {
        snprintf(buf, sizeof(buf), "%.6f", gpsLat()); doc["lat"] = buf;
        snprintf(buf, sizeof(buf), "%.6f", gpsLon()); doc["lon"] = buf;
    }
    doc["sats"] = (int)gpsSats();
    if (gpsSpeedValid())  { snprintf(buf, sizeof(buf), "%.1f", gpsSpeedKnots()); doc["speed_kts"] = buf; }
    if (gpsCourseValid()) { snprintf(buf, sizeof(buf), "%.1f", gpsCourseDeg());  doc["course"]    = buf; }

    // Waypoint
    doc["wp_set"]   = navWpSet();
    doc["captured"] = navCaptured();
    doc["approach_lock"] = navApproachLocked();
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

    // PID (current live values)
    snprintf(buf, sizeof(buf), "%.2f", pidKp()); doc["pid_kp"] = buf;
    snprintf(buf, sizeof(buf), "%.2f", pidKd()); doc["pid_kd"] = buf;

    // ── Mag calibration / health ─────────────────────────────────────────
    doc["mag_cal_state"]    = imuMagCalStateName();
    doc["mag_cal_progress"] = imuMagCalProgressPct();
    doc["mag_calibrated"]   = imuMagCalibrated();
    doc["mag_cal_ts"]       = imuMagCalTs();
    doc["mag_from_nvs"]     = imuMagFromNVS();
    if (imuMagCalCollecting()) doc["mag_cal_mask"] = imuMagCalMask();
    if (imuMagCalFailed())     doc["mag_cal_fail"] = imuMagCalFailReason();
    doc["mag_cal_quality"] = imuMagCalQualityName();
    if (imuMagCalQualityKnown()) {
        snprintf(buf, sizeof(buf), "%.1f", imuMagCalRadiusUT()); doc["mag_cal_radius_uT"] = buf;
        snprintf(buf, sizeof(buf), "%.0f", imuMagCalCircPct());  doc["mag_cal_circ_pct"]  = buf;
    }
    snprintf(buf, sizeof(buf), "%.2f", imuMagOffX());       doc["mag_off_x"]       = buf;
    snprintf(buf, sizeof(buf), "%.2f", imuMagOffY());       doc["mag_off_y"]       = buf;
    snprintf(buf, sizeof(buf), "%.2f", imuMagOffZ());       doc["mag_off_z"]       = buf;
    snprintf(buf, sizeof(buf), "%.2f", imuMagBaselineUT()); doc["mag_baseline_uT"] = buf;
    snprintf(buf, sizeof(buf), "%.2f", imuLiveMagUT());     doc["mag_uT"]          = buf;

    // A truncated payload is invalid JSON the app can't parse — warn (only
    // near the edge) so the operator isn't silently blinded.
    size_t jsonLen = measureJson(doc);
    if (jsonLen > 1800) Serial.printf("[telemetry] WARN json=%u bytes near 2048 buffer\n", (unsigned)jsonLen);

    char out[2048];
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
            char err[80];
            snprintf(err, sizeof(err),
                "{\"ok\":false,\"err\":\"us out of [%u..%u]\"}", NEUTRAL_US, MAX_FWD_US);
            server.send(400, "application/json", err);
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

    Command c = {};
    c.type     = CMD_CRUISE;
    c.cruiseUs = newUs;
    if (!cmdEnqueue(c)) { server.send(503, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }

    StaticJsonDocument<128> resp;
    resp["ok"]        = true;
    resp["cruise_us"] = newUs;                 // requested value; applied within one control cycle
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

    // {"lat":null,"lon":null} → clear waypoint and reset capture latch.
    if (req["lat"].isNull() || req["lon"].isNull()) {
        Command c = {}; c.type = CMD_WAYPOINT_CLEAR;
        if (!cmdEnqueue(c)) { server.send(503, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }
        server.send(200, "application/json", "{\"ok\":true,\"cleared\":true}");
        return;
    }
    if (!req.containsKey("lat") || !req.containsKey("lon")) {
        server.send(400, "text/plain", "Need lat and lon");
        return;
    }
    float lat = req["lat"].as<float>();
    float lon = req["lon"].as<float>();
    float d   = 0.0f;
    // Validate (read-only) here; the control loop performs the actual set.
    if (!navWaypointInRange(lat, lon, &d)) {
        char err[96];
        snprintf(err, sizeof(err),
            "{\"ok\":false,\"err\":\"waypoint %.0f m away (max %.0f m)\"}", d, MAX_WP_DIST_M);
        server.send(400, "application/json", err);
        return;
    }
    Command c = {}; c.type = CMD_WAYPOINT_SET; c.lat = lat; c.lon = lon;
    if (!cmdEnqueue(c)) { server.send(503, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }

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
            server.send(400, "application/json", "{\"ok\":false,\"err\":\"kp out of [0..20]\"}");
            return;
        }
    }
    if (req.containsKey("kd")) {
        kd = req["kd"].as<float>();
        if (kd < 0.0f || kd > 30.0f) {
            server.send(400, "application/json", "{\"ok\":false,\"err\":\"kd out of [0..30]\"}");
            return;
        }
    }
    Command c = {};
    c.type = CMD_PID;
    c.kp   = kp;     // resolved against current above; loop applies both
    c.kd   = kd;
    if (!cmdEnqueue(c)) { server.send(503, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }

    StaticJsonDocument<128> resp;
    resp["ok"] = true;
    resp["kp"] = kp;                           // requested values; applied within one control cycle
    resp["kd"] = kd;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleCalibrateMagStart() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    if (imuMagCalCollecting()) {
        server.send(200, "application/json",
            "{\"ok\":true,\"state\":\"collecting\",\"note\":\"already running\"}");
        return;
    }
    Command c = {}; c.type = CMD_MAGCAL_START;
    if (!cmdEnqueue(c)) { server.send(503, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }
    server.send(200, "application/json", "{\"ok\":true,\"state\":\"collecting\"}");
}

static void handleCalibrateMagAbort() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    if (!imuMagCalCollecting()) {
        server.send(200, "application/json",
            "{\"ok\":true,\"state\":\"idle\",\"note\":\"not collecting\"}");
        return;
    }
    Command c = {}; c.type = CMD_MAGCAL_ABORT;
    if (!cmdEnqueue(c)) { server.send(503, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }
    server.send(200, "application/json", "{\"ok\":true,\"state\":\"idle\"}");
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

    Command c = {}; c.type = CMD_LED; c.ledId = (uint8_t)id; c.ledOn = on;
    if (!cmdEnqueue(c)) { server.send(503, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleAudio() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    if (!audioAvailable()) {
        server.send(503, "application/json", "{\"ok\":false,\"err\":\"DF1201S not initialised\"}");
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
    Command c = {}; c.type = CMD_BILGE; c.bilgeOn = on;
    if (!cmdEnqueue(c)) { server.send(503, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }

    StaticJsonDocument<128> resp;
    resp["ok"]          = true;
    resp["pump_manual"] = on;                  // requested value; applied within one control cycle
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

    if (req.containsKey("on")) on = req["on"].as<bool>();
    if (req.containsKey("speed")) {
        int v = req["speed"].as<int>();
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        s = (uint8_t)v;
    }
    if (req.containsKey("burst_ms")) b = (uint32_t)req["burst_ms"].as<long>();
    if (req.containsKey("pause_ms")) p = (uint32_t)req["pause_ms"].as<long>();

    Command c = {};
    c.type = CMD_RADAR;
    c.radarOn = on; c.radarSpeed = s; c.radarBurstMs = b; c.radarPauseMs = p;
    if (!cmdEnqueue(c)) { server.send(503, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }

    StaticJsonDocument<160> resp;
    resp["ok"]       = true;                   // resolved values; applied within one control cycle
    resp["on"]       = on;
    resp["speed"]    = s;
    resp["burst_ms"] = b;
    resp["pause_ms"] = p;
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
    Command c = {};
    const char* respMode;
    if      (!strcmp(m, "stop")) { c.type = CMD_DEPTH_MODE; c.depthRun = false; respMode = "off"; }
    else if (!strcmp(m, "run"))  { c.type = CMD_DEPTH_MODE; c.depthRun = true;  respMode = "run"; }
    else if (!strcmp(m, "check")){ c.type = CMD_DEPTH_PING; respMode = (sonarMode() == DEPTH_RUN) ? "run" : "off"; }
    else {
        server.send(400, "application/json", "{\"ok\":false,\"err\":\"mode must be stop|check|run\"}");
        return;
    }
    if (!cmdEnqueue(c)) { server.send(503, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }

    StaticJsonDocument<128> resp;
    resp["ok"]   = true;
    resp["mode"] = respMode;
    if (sonarLastDepthM() >= 0.0f) {
        char dbuf[12];
        snprintf(dbuf, sizeof(dbuf), "%.2f", sonarLastDepthM());
        resp["depth_m"] = dbuf;
    }
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

// ── /history (store-and-sync gap backfill) ──────────────────────────────────
// GET /history?since_ms=<uptimeMs> → records with uptimeMs > since_ms, oldest
// first, capped at HISTLOG_PAGE_MAX. Response:
//   {"session_id":N,"more":bool,"records":[ {…compact telemetry…}, … ]}
// The app pages by re-requesting with since_ms = the last record's uptime_ms,
// and finalizes the flight (rather than backfilling) when session_id changes.
// Record keys + value types mirror /telemetry so the app slots them straight
// into the same flight log; invalid fields are omitted, exactly as live.
static void appendHistRecord(String& out, const HistRecord* r) {
    char buf[320];
    const char* modeStr = (r->mode == 1) ? "MANUAL"
                        : (r->mode == 2) ? "AUTO"
                        : (r->mode == 3) ? "FAILSAFE" : "IDLE";
    bool fix = r->flags & HIST_F_GPS_FIX;
    snprintf(buf, sizeof(buf),
        "{\"seq\":%lu,\"uptime_ms\":%lu,\"mode\":\"%s\","
        "\"esc_us\":%d,\"rudder_us\":%d,\"heading\":\"%.1f\","
        "\"gps_fix\":%s,\"sats\":%u",
        (unsigned long)r->seq, (unsigned long)r->uptimeMs, modeStr,
        r->escUs, r->rudderUs, r->heading10 / 10.0f,
        fix ? "true" : "false", r->sats);
    out += buf;

    if (fix) {
        snprintf(buf, sizeof(buf), ",\"lat\":\"%.6f\",\"lon\":\"%.6f\"",
                 r->lat1e7 / 1e7, r->lon1e7 / 1e7);
        out += buf;
    }
    if (r->speedKts100 != INT16_MIN) {
        snprintf(buf, sizeof(buf), ",\"speed_kts\":\"%.1f\"", r->speedKts100 / 100.0f); out += buf;
    }
    if (r->course10 != INT16_MIN) {
        snprintf(buf, sizeof(buf), ",\"course\":\"%.1f\"", r->course10 / 10.0f); out += buf;
    }
    if (r->battCv != INT16_MIN) {
        snprintf(buf, sizeof(buf), ",\"batt_v\":\"%.2f\",\"batt_a\":\"%.2f\"",
                 r->battCv / 100.0f, r->battCa / 100.0f);
        out += buf;
    }
    if (r->depthCm >= 0) {
        snprintf(buf, sizeof(buf), ",\"depth_m\":\"%.2f\"", r->depthCm / 100.0f); out += buf;
    }
    snprintf(buf, sizeof(buf), ",\"wp_set\":%s,\"captured\":%s",
             (r->flags & HIST_F_WP_SET)  ? "true" : "false",
             (r->flags & HIST_F_CAPTURED) ? "true" : "false");
    out += buf;
    if (r->wpDist10 >= 0) {
        snprintf(buf, sizeof(buf), ",\"wp_dist_m\":\"%.1f\"", r->wpDist10 / 10.0f); out += buf;
    }
    snprintf(buf, sizeof(buf), ",\"pump\":%s,\"failsafe_ack\":%s}",
             (r->flags & HIST_F_PUMP)         ? "true" : "false",
             (r->flags & HIST_F_FAILSAFE_ACK) ? "true" : "false");
    out += buf;
}

static void handleHistory() {
    addCORS();

    uint32_t since = 0;
    if (server.hasArg("since_ms")) since = strtoul(server.arg("since_ms").c_str(), NULL, 10);

    // Chunked send so we never build the whole page in one big buffer.
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");

    char head[80];
    snprintf(head, sizeof(head), "{\"session_id\":%lu,\"more\":", (unsigned long)sessionId);

    uint16_t n = histlogCount();
    uint16_t sent = 0;
    bool     more = false;
    String   recsOut;                 // accumulates a few records, flushed in batches
    recsOut.reserve(2048);

    // Decide `more` up front so it can go in the header chunk: count how many
    // records qualify; if that exceeds the page cap, there's a next page.
    for (uint16_t i = 0; i < n; i++) {
        const HistRecord* r = histlogAt(i);
        if (r && r->uptimeMs > since) {
            if (sent >= HISTLOG_PAGE_MAX) { more = true; break; }
            sent++;
        }
    }

    server.sendContent(head);
    server.sendContent(more ? "true,\"records\":[" : "false,\"records\":[");

    uint16_t emitted = 0;
    for (uint16_t i = 0; i < n && emitted < HISTLOG_PAGE_MAX; i++) {
        const HistRecord* r = histlogAt(i);
        if (!r || r->uptimeMs <= since) continue;
        if (emitted > 0) recsOut += ',';
        appendHistRecord(recsOut, r);
        emitted++;
        if (recsOut.length() > 1536) { server.sendContent(recsOut); recsOut = ""; }
    }
    if (recsOut.length()) server.sendContent(recsOut);
    server.sendContent("]}");
    server.sendContent("");           // terminate chunked response
}

void telemetryBegin() {
    sessionId = esp_random();   // app uses this to detect mid-flight reboots
    wifiConnect();

    server.on("/status",    HTTP_GET,     handleStatus);
    server.on("/status",    HTTP_OPTIONS, handleOptions);
    server.on("/telemetry", HTTP_GET,     handleTelemetry);
    server.on("/telemetry", HTTP_OPTIONS, handleOptions);
    server.on("/history",   HTTP_GET,     handleHistory);
    server.on("/history",   HTTP_OPTIONS, handleOptions);
    server.on("/cruise",    HTTP_POST,    handleCruise);
    server.on("/cruise",    HTTP_OPTIONS, handleOptions);
    server.on("/waypoint",  HTTP_POST,    handleWaypoint);
    server.on("/waypoint",  HTTP_OPTIONS, handleOptions);
    server.on("/pid",       HTTP_POST,    handlePid);
    server.on("/pid",       HTTP_OPTIONS, handleOptions);
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
    server.on("/calibrate_mag/start", HTTP_POST,    handleCalibrateMagStart);
    server.on("/calibrate_mag/start", HTTP_OPTIONS, handleOptions);
    server.on("/calibrate_mag/abort", HTTP_POST,    handleCalibrateMagAbort);
    server.on("/calibrate_mag/abort", HTTP_OPTIONS, handleOptions);
    server.begin();
    if (boatIP.length()) {
        Serial.printf("HTTP up at http://%s/\n", boatIP.c_str());
    }

    // Hand the server to a core-0 task. From here on, handleClient()/wifiMaintain()
    // run ONLY here — never on the control loop (core 1) — so a blocked socket
    // send on a bad link can never stall RC/FSM/failsafe. server is touched only
    // by this task after this point.
    xTaskCreatePinnedToCore(networkTask, "net", NET_TASK_STACK, NULL, 1, &netTaskHandle, NET_TASK_CORE);
}

// Owns all networking. A blocking send here delays only telemetry (expendable);
// the control loop is on the other core and never waits behind it.
static void networkTask(void*) {
    for (;;) {
        server.handleClient();
        wifiMaintain();
        vTaskDelay(1);          // yield ~1 tick; telemetry is best-effort
    }
}
