/*
 * test_28_multi_waypoint.ino
 *
 * Bench-only multi-waypoint mission. Proves the SEQUENCER (storage, capture
 * detection, advance, mission complete) using HTTP-injected fake GPS
 * positions. No physical motion required — Ryan's yard isn't large enough
 * to walk the boat through real GPS waypoints.
 *
 * Inherits from test_27 (canonical Mode FSM, CH6 SwD failsafe guard,
 * sticky ACK, /cruise, /telemetry, INA219). The only NEW capability is
 * mission state + sim_gps + sequencer.
 *
 * Channel map (locked 2026-05-10):
 *   CH1 (idx 0)  rudder           CH5 (idx 4)  knob
 *   CH2 (idx 1)  reverse          CH6 (idx 5)  SwD failsafe guard
 *   CH3 (idx 2)  throttle         CH7 (idx 6)  SwA mode (up=MANUAL,
 *   CH4 (idx 3)  unused                          down=AUTO)
 *
 * What's NEW vs test_27:
 *   - Mission storage (volatile, up to 32 waypoints).
 *   - HTTP /mission (POST array), /mission/clear, /sim_gps.
 *   - Sequencer: when dist(boat, waypoints[wp_idx]) < 3 m, advance wp_idx;
 *     when wp_idx >= wp_count, mission_active=false, log MISSION COMPLETE
 *     once.
 *   - Telemetry adds wp_idx, wp_count, wp_dist_m, wp_bearing,
 *     mission_active, gps_simulated, lat, lon.
 *
 * Divergences from the handoff prompt's test_28 spec (deliberate):
 *   1. No /mission/start or /mission/stop. Mission auto-arms on POST.
 *      Re-POST to restart. One fewer endpoint.
 *   2. AUTO requires a mission. Without an active mission OR without a
 *      valid position, AUTO neutralizes outputs (test_27's "AUTO = cruise
 *      + heading hold at entry heading" was a stand-in for a sketch with
 *      no mission concept; from here AUTO means "drive the mission").
 *   3. Sequencer runs regardless of mode. wp_idx advances on capture
 *      whether MANUAL or AUTO — only the rudder/ESC behavior is
 *      mode-gated. Lets us prove the sequencer with motors silent.
 *   4. FAILSAFE during a mission PAUSES it. wp_idx and mission_active
 *      survive the failsafe round-trip. Simpler than "resume vs restart".
 *   5. All gates auto-detected; no y/n prompts (per feedback_test_style).
 *
 * Gate flow (auto-detected, sketch prompts each phase ONCE):
 *   1/4  POST /mission with >= 1 waypoint → wp_count and wp_idx visible.
 *   2/4  After sim_gps far→near to WP1 → wp_idx 0→1.
 *   3/4  Mission completion (wp_idx == wp_count) → mission_active=false,
 *        MISSION COMPLETE printed once. If at completion mode==AUTO,
 *        ESCs are also verified neutral.
 *   4/4  In a fresh mission, kill TX during AUTO → MODE_FAILSAFE; restore
 *        TX, ACK via SwA UP → DOWN; mission_active still true and wp_idx
 *        unchanged from when failsafe tripped.
 *
 * Hardware (additions to test_27):
 *   BN-220 GPS  → UART2  RX=GPIO17 (← white)  TX=GPIO4 (→ green)
 *                 Reversed wire colors confirmed in this batch.
 *
 * SAFETY:
 *   PROPS OFF or boat firmly secured. AUTO cruise floor 1650 µs, cap
 *   1750 µs. Hard ESC clamp at MAX_FWD_US=1800 in setEscs().
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_INA219.h>
#include "ICM_20948.h"
#include <TinyGPSPlus.h>
#include <math.h>
#include "secrets.h"

// ── PCA9685 channels ────────────────────────────────────────────────────────
static const uint8_t  CH_ESC_PORT = 0;
static const uint8_t  CH_ESC_STBD = 1;
static const uint8_t  CH_RUDDER   = 2;

// ── Output bounds ───────────────────────────────────────────────────────────
static const uint16_t RUDDER_MIN_US = 1330;
static const uint16_t RUDDER_MAX_US = 1670;
static const uint16_t NEUTRAL_US    = 1500;
static const uint16_t MAX_FWD_US    = 1800;

// ── AUTO cruise selection ───────────────────────────────────────────────────
static const uint16_t AUTO_CRUISE_FLOOR_US = 1650;
static const uint16_t AUTO_CRUISE_CAP_US   = 1750;
static const uint16_t DEFAULT_CRUISE_US    = 1660;

// ── iBUS channel indices (locked 2026-05-10) ────────────────────────────────
static const uint8_t  IBUS_IDX_RUDDER         = 0;
static const uint8_t  IBUS_IDX_THROTTLE       = 2;
static const uint8_t  IBUS_IDX_FAILSAFE_GUARD = 5;
static const uint8_t  IBUS_IDX_MODE           = 6;
static const uint8_t  IBUS_RX_PIN             = 16;

// ── SwA hysteresis ──────────────────────────────────────────────────────────
static const uint16_t MODE_MAN_BELOW_US  = 1450;
static const uint16_t MODE_AUTO_ABOVE_US = 1550;

// ── Stick safe-arm ──────────────────────────────────────────────────────────
static const uint16_t THROTTLE_IDLE_MAX = 1100;
static const uint16_t STICK_NEUTRAL_DB  = 30;

// ── Failsafe ────────────────────────────────────────────────────────────────
static const uint16_t FAILSAFE_GUARD_THRESHOLD = 1500;
static const uint32_t FAILSAFE_DETECT_MS       = 500;
static const uint32_t FAILSAFE_NO_FRAME_MS     = 3000;

// ── IMU + heading hold ──────────────────────────────────────────────────────
static const uint32_t IMU_UPDATE_INTERVAL_MS = 20;
static const float    Kp = 3.0f;
static const float    Kd = 8.0f;
static const float    MAG_OFFSET_X = -20.70f;
static const float    MAG_OFFSET_Y =  -0.45f;
static const float    MAG_OFFSET_Z = -17.70f;
static const float    ALPHA = 0.98f;

// ── INA219 ──────────────────────────────────────────────────────────────────
static const uint8_t INA219_ADDR = 0x41;

// ── GPS ─────────────────────────────────────────────────────────────────────
static const uint8_t  GPS_RX_PIN = 17;
static const uint8_t  GPS_TX_PIN = 4;
static const uint32_t GPS_BAUD   = 9600;

// ── Mission ─────────────────────────────────────────────────────────────────
// 32 was chosen to match the AUTOPILOT_PLAN scaffold; this sketch never
// approaches it on the bench (gate flow uses 3 waypoints).
static const uint8_t  MAX_WAYPOINTS    = 32;
static const float    CAPTURE_RADIUS_M = 3.0f;

// ── Hardware ───────────────────────────────────────────────────────────────
static Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
static Adafruit_INA219          ina219(INA219_ADDR);
static ICM_20948_I2C            myICM;
static TinyGPSPlus              gps;
static HardwareSerial           ibusSerial(1);
static HardwareSerial           gpsSerial(2);
static WebServer                server(80);
static String                   boatIP;
static bool                     ina219OK = false;

// ── Mode state machine ─────────────────────────────────────────────────────
enum Mode { MODE_MANUAL, MODE_AUTO, MODE_FAILSAFE };
static Mode    mode                = MODE_MANUAL;
static bool    failsafeAckRequired = false;
static bool    ackRefusalPrinted   = false;
static uint16_t cruiseUs           = DEFAULT_CRUISE_US;

// ── Mission state ──────────────────────────────────────────────────────────
struct Waypoint { float lat; float lon; };
static Waypoint waypoints[MAX_WAYPOINTS];
static uint8_t  wpCount      = 0;
static uint8_t  wpIdx        = 0;
static bool     missionActive = false;

// Cached per-loop values (telemetry + sequencer share these).
static float    activeBearing = 0.0f;
static float    activeDistM   = 0.0f;

// ── Position state ──────────────────────────────────────────────────────────
// gpsSimulated is sticky: once /sim_gps is POSTed in this session, real GPS
// is ignored. Lets a bench operator inject positions deterministically.
static float    boatLat       = 0.0f;
static float    boatLon       = 0.0f;
static bool     gpsValid      = false;
static bool     gpsSimulated  = false;

// ── Failsafe-during-mission tracking (gate 4) ──────────────────────────────
// When mode transitions INTO FAILSAFE, snapshot whether a mission was active
// and what wp_idx was. Gate 4 passes when AUTO is re-entered with mission
// still active and wp_idx unchanged from that snapshot.
static bool     missionActiveAtFs = false;
static uint8_t  wpIdxAtFs         = 0;
static bool     fsDuringMission   = false;

// ── Test phase ──────────────────────────────────────────────────────────────
enum Phase {
    P_WAIT_MISSION,   // gate 1: waiting for POST /mission
    P_WAIT_ADVANCE,   // gate 2: waiting for first wp_idx advancement
    P_WAIT_COMPLETE,  // gate 3: waiting for mission completion
    P_WAIT_FAILSAFE,  // gate 4: waiting for failsafe-during-mission cycle
    P_DONE
};
static Phase phase = P_WAIT_MISSION;
static bool  g1Pass = false, g2Pass = false, g3Pass = false, g4Pass = false;

// ── iBUS / RC state ─────────────────────────────────────────────────────────
static uint8_t  ibusBuf[32], ibusIdx = 0;
static uint16_t ch[10] = {0};
static bool     ibusEverGood      = false;
static uint32_t lastFrameMs       = 0;
static uint32_t guardAboveSinceMs = 0;

// ── Output state ────────────────────────────────────────────────────────────
static uint16_t outRudder = NEUTRAL_US, outPort = NEUTRAL_US, outStbd = NEUTRAL_US;

// ── IMU / heading hold state ────────────────────────────────────────────────
static float    fusedHeading    = 0;
static float    prevHeadingForD = 0;
static uint32_t lastDtUs        = 0;
static unsigned long lastImuUs  = 0;
static bool     headingInit     = false;

// ── INA219 cache ────────────────────────────────────────────────────────────
static float    busVoltage = 0.0f;
static float    shuntMa    = 0.0f;
static uint32_t lastInaPollMs = 0;
static const uint32_t INA_POLL_INTERVAL_MS = 250;

// ── Helpers ────────────────────────────────────────────────────────────────
static uint16_t usTicks(uint16_t us) { return (uint16_t)((us / 20000.0f) * 4096); }
static void writePCA(uint8_t c, uint16_t us) { pca.setPWM(c, 0, usTicks(us)); }

static void setRudder(uint16_t us) {
    if (us < RUDDER_MIN_US) us = RUDDER_MIN_US;
    if (us > RUDDER_MAX_US) us = RUDDER_MAX_US;
    outRudder = us;
    writePCA(CH_RUDDER, us);
}
static void setEscs(uint16_t us) {
    if (us < NEUTRAL_US) us = NEUTRAL_US;
    if (us > MAX_FWD_US) us = MAX_FWD_US;
    outPort = outStbd = us;
    writePCA(CH_ESC_PORT, us);
    writePCA(CH_ESC_STBD, us);
}

static uint16_t mapThrottleStickToEsc(uint16_t stickUs) {
    if (stickUs <= THROTTLE_IDLE_MAX) return NEUTRAL_US;
    if (stickUs >= 2000) return MAX_FWD_US;
    return (uint16_t)map(stickUs, THROTTLE_IDLE_MAX, 2000, NEUTRAL_US, MAX_FWD_US);
}
static uint16_t mapRudderStickToServo(uint16_t stickUs) {
    if (stickUs < 1000) stickUs = 1000;
    if (stickUs > 2000) stickUs = 2000;
    if (stickUs <= 1500) return (uint16_t)map(stickUs, 1000, 1500, RUDDER_MIN_US, NEUTRAL_US);
    return                       (uint16_t)map(stickUs, 1500, 2000, NEUTRAL_US, RUDDER_MAX_US);
}

static float shortestPathError(float t, float c) {
    float e = t - c;
    while (e >  180.0f) e -= 360.0f;
    while (e < -180.0f) e += 360.0f;
    return e;
}

static bool throttleAtIdle() { return ch[IBUS_IDX_THROTTLE] <= THROTTLE_IDLE_MAX; }
static bool rudderCentered() { return abs((int)ch[IBUS_IDX_RUDDER] - NEUTRAL_US) <= STICK_NEUTRAL_DB; }
static bool swcInManual()    { return ch[IBUS_IDX_MODE] < MODE_MAN_BELOW_US; }
static bool swcInAuto()      { return ch[IBUS_IDX_MODE] > MODE_AUTO_ABOVE_US; }

static const char* modeName(Mode m) {
    switch (m) {
      case MODE_MANUAL:   return "MANUAL";
      case MODE_AUTO:     return "AUTO";
      case MODE_FAILSAFE: return "FAILSAFE";
    }
    return "?";
}

// ── Haversine (lifted from test_25, proven) ─────────────────────────────────
static float haversineBearing(float fromLat, float fromLon, float toLat, float toLon) {
    float p1 = fromLat * DEG_TO_RAD;
    float p2 = toLat   * DEG_TO_RAD;
    float dl = (toLon - fromLon) * DEG_TO_RAD;
    float y  = sinf(dl) * cosf(p2);
    float x  = cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl);
    float b  = atan2f(y, x) * RAD_TO_DEG;
    return fmodf(b + 360.0f, 360.0f);
}
static float haversineDistM(float fromLat, float fromLon, float toLat, float toLon) {
    const float R = 6371000.0f;
    float p1 = fromLat * DEG_TO_RAD;
    float p2 = toLat   * DEG_TO_RAD;
    float dp = (toLat  - fromLat) * DEG_TO_RAD;
    float dl = (toLon  - fromLon) * DEG_TO_RAD;
    float a  = sinf(dp / 2) * sinf(dp / 2)
             + cosf(p1) * cosf(p2) * sinf(dl / 2) * sinf(dl / 2);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

// ── iBUS parsing ───────────────────────────────────────────────────────────
static bool parseIbus() {
    if (ibusBuf[0] != 0x20 || ibusBuf[1] != 0x40) return false;
    uint16_t sum = 0xFFFF;
    for (int i = 0; i < 30; i++) sum -= ibusBuf[i];
    uint16_t rx = ibusBuf[30] | (ibusBuf[31] << 8);
    if (sum != rx) return false;
    for (int i = 0; i < 10; i++)
        ch[i] = ibusBuf[2 + i*2] | (ibusBuf[3 + i*2] << 8);
    lastFrameMs = millis();
    if (!ibusEverGood) ibusEverGood = true;
    return true;
}
static void readIbus() {
    while (ibusSerial.available()) {
        uint8_t b = ibusSerial.read();
        if (ibusIdx == 0 && b != 0x20) continue;
        ibusBuf[ibusIdx++] = b;
        if (ibusIdx == 32) { parseIbus(); ibusIdx = 0; }
    }
}

// ── IMU + complementary filter (same as test_27) ───────────────────────────
static void updateImu() {
    static uint32_t lastImuPollMs = 0;
    if (millis() - lastImuPollMs < IMU_UPDATE_INTERVAL_MS) return;
    if (!myICM.dataReady()) return;
    lastImuPollMs = millis();
    myICM.getAGMT();
    float ax = myICM.accX(), ay = myICM.accY(), az = myICM.accZ();
    float gx = myICM.gyrX(), gy = myICM.gyrY(), gz = myICM.gyrZ();
    float mx = myICM.magX() - MAG_OFFSET_X;
    float my = myICM.magY() - MAG_OFFSET_Y;
    float mz = myICM.magZ() - MAG_OFFSET_Z;

    float ar_x = ay, ar_y = az, ar_z = ax;
    float mr_x = -mz, mr_y = -my, mr_z = -mx;
    float roll  = atan2f(ar_y, ar_z);
    float pitch = atan2f(-ar_x, sqrtf(ar_y*ar_y + ar_z*ar_z));
    float Bx = mr_x*cosf(pitch) + mr_y*sinf(roll)*sinf(pitch) + mr_z*cosf(roll)*sinf(pitch);
    float By = mr_y*cosf(roll) - mr_z*sinf(roll);
    float magH = atan2f(-By, Bx) * 180.0f / PI;
    if (magH < 0) magH += 360.0f;
    float accelMag = sqrtf(ax*ax + ay*ay + az*az);

    unsigned long nowUs = micros();
    float dt = (lastImuUs == 0) ? 0.0f : (nowUs - lastImuUs) * 1e-6f;
    lastImuUs = nowUs;
    if (!headingInit) {
        fusedHeading = magH;
        prevHeadingForD = magH;
        headingInit = true;
    } else {
        float yawRate = (accelMag > 100.0f) ? (ax*gx + ay*gy + az*gz)/accelMag : 0.0f;
        float gyroH = fusedHeading + yawRate * dt;
        while (gyroH <   0.0f) gyroH += 360.0f;
        while (gyroH >= 360.0f) gyroH -= 360.0f;
        float diff = magH - gyroH;
        if (diff >  180.0f) diff -= 360.0f;
        if (diff < -180.0f) diff += 360.0f;
        fusedHeading = gyroH + (1.0f - ALPHA) * diff;
        if (fusedHeading <   0.0f) fusedHeading += 360.0f;
        if (fusedHeading >= 360.0f) fusedHeading -= 360.0f;
    }
}

static uint16_t headingHoldUs(float target) {
    if (!headingInit) return NEUTRAL_US;
    float err = shortestPathError(target, fusedHeading);
    uint32_t nowUs = micros();
    float dt = (lastDtUs == 0) ? 0.0f : (nowUs - lastDtUs) * 1e-6f;
    lastDtUs = nowUs;
    float dErr = 0.0f;
    if (dt > 0.0f && dt < 0.5f) {
        float dH = shortestPathError(fusedHeading, prevHeadingForD);
        dErr = -dH / dt;
    }
    prevHeadingForD = fusedHeading;
    int v = (int)(NEUTRAL_US + Kp * err + Kd * dErr);
    if (v < RUDDER_MIN_US) v = RUDDER_MIN_US;
    if (v > RUDDER_MAX_US) v = RUDDER_MAX_US;
    return (uint16_t)v;
}

// ── INA219 ─────────────────────────────────────────────────────────────────
static void pollIna219() {
    if (!ina219OK) return;
    if (millis() - lastInaPollMs < INA_POLL_INTERVAL_MS) return;
    lastInaPollMs = millis();
    busVoltage = ina219.getBusVoltage_V() + (ina219.getShuntVoltage_mV() / 1000.0f);
    shuntMa    = ina219.getCurrent_mA();
}

// ── Position update ────────────────────────────────────────────────────────
// sim wins if it's ever been set this session. Real GPS is read-and-discard
// once gpsSimulated is true so the UART buffer doesn't overflow.
static void updatePosition() {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    if (gpsSimulated) return;
    if (gps.location.isValid() && gps.location.age() < 5000) {
        boatLat  = (float)gps.location.lat();
        boatLon  = (float)gps.location.lng();
        gpsValid = true;
    }
    // Note: not clearing gpsValid on real-GPS staleness — GPS-loss failsafe
    // is deferred (AUTOPILOT_PLAN sea trial).
}

// ── Sequencer ──────────────────────────────────────────────────────────────
// Runs every loop. Updates activeBearing / activeDistM for telemetry, and
// advances wp_idx on capture. Mode-agnostic: capture detection works the
// same in MANUAL or AUTO so the sequencer can be exercised with motors
// silent.
static void updateSequencer() {
    if (!missionActive || !gpsValid || wpIdx >= wpCount) {
        // Leave activeBearing / activeDistM as-is; not meaningful when
        // mission isn't active. Telemetry surfaces missionActive so the
        // operator can disambiguate.
        return;
    }
    Waypoint& w = waypoints[wpIdx];
    activeBearing = haversineBearing(boatLat, boatLon, w.lat, w.lon);
    activeDistM   = haversineDistM (boatLat, boatLon, w.lat, w.lon);

    if (activeDistM < CAPTURE_RADIUS_M) {
        uint8_t prev = wpIdx;
        wpIdx++;
        if (wpIdx >= wpCount) {
            missionActive = false;
            Serial.printf("[MISSION] WP %u captured (%.1f m). MISSION COMPLETE.\n",
                          prev, activeDistM);
        } else {
            Serial.printf("[MISSION] WP %u → %u (captured at %.1f m).\n",
                          prev, wpIdx, activeDistM);
        }
    }
}

// ── WiFi (same as test_27) ──────────────────────────────────────────────────
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

static void wifiSetup() {
    WiFi.mode(WIFI_STA);
    if (tryConnect(SECRET_HOME_SSID,    SECRET_HOME_PASS,    10)) return;
    if (tryConnect(SECRET_HOTSPOT_SSID, SECRET_HOTSPOT_PASS, 10)) return;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("LegendCutter", "coastguard");
    boatIP = WiFi.softAPIP().toString();
    Serial.printf("[WiFi] AP fallback  IP: %s\n", boatIP.c_str());
}

// ── HTTP handlers ──────────────────────────────────────────────────────────
static void addCORS() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}
static void handleOptions() { addCORS(); server.send(204); }

static void handleStatus() {
    addCORS();
    server.send(200, "application/json",
        "{\"ok\":true,\"v\":\"test_28\",\"ip\":\"" + boatIP + "\"}");
}

static void handleTelemetry() {
    addCORS();
    StaticJsonDocument<768> doc;
    doc["v"]            = "test_28";
    doc["uptime"]       = millis() / 1000;
    doc["heap"]         = ESP.getFreeHeap();
    doc["mode"]         = modeName(mode);
    doc["cruise_us"]    = cruiseUs;
    doc["failsafe_ack"] = failsafeAckRequired;
    doc["rc_ever_good"] = ibusEverGood;
    doc["rc_age_ms"]    = ibusEverGood ? (millis() - lastFrameMs) : 0;
    doc["rudder_us"]    = outRudder;
    doc["esc_us"]       = outPort;
    doc["ch_throttle"]  = ch[IBUS_IDX_THROTTLE];
    doc["ch_rudder"]    = ch[IBUS_IDX_RUDDER];
    doc["ch_mode"]      = ch[IBUS_IDX_MODE];
    doc["ch_guard"]     = ch[IBUS_IDX_FAILSAFE_GUARD];

    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f", fusedHeading); doc["heading"] = buf;
    if (ina219OK) {
        snprintf(buf, sizeof(buf), "%.2f", busVoltage); doc["bus_v"]    = buf;
        snprintf(buf, sizeof(buf), "%.0f", shuntMa);    doc["shunt_ma"] = buf;
    }

    // Position
    doc["gps_valid"]     = gpsValid;
    doc["gps_simulated"] = gpsSimulated;
    if (gpsValid) {
        snprintf(buf, sizeof(buf), "%.6f", boatLat); doc["lat"] = buf;
        snprintf(buf, sizeof(buf), "%.6f", boatLon); doc["lon"] = buf;
    }

    // Mission
    doc["mission_active"] = missionActive;
    doc["wp_count"]       = wpCount;
    doc["wp_idx"]         = wpIdx;
    if (missionActive && gpsValid && wpIdx < wpCount) {
        snprintf(buf, sizeof(buf), "%.1f", activeDistM);   doc["wp_dist_m"]   = buf;
        snprintf(buf, sizeof(buf), "%.1f", activeBearing); doc["wp_bearing"]  = buf;
    }

    char out[768];
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleCruise() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }

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
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        newUs = (uint16_t)(NEUTRAL_US + (MAX_FWD_US - NEUTRAL_US) * (pct / 100.0f));
    } else {
        server.send(400, "text/plain", "Need 'us' or 'pct'");
        return;
    }

    cruiseUs = newUs;
    Serial.printf("[HTTP] /cruise → %u µs%s\n",
        cruiseUs,
        (cruiseUs < AUTO_CRUISE_FLOOR_US) ? "  (BELOW FLOOR — AUTO will refuse)" :
        (cruiseUs > AUTO_CRUISE_CAP_US)   ? "  (ABOVE CAP — will clamp on engage)" : "");

    StaticJsonDocument<128> resp;
    resp["ok"]        = true;
    resp["cruise_us"] = cruiseUs;
    resp["floor"]     = AUTO_CRUISE_FLOOR_US;
    resp["cap"]       = AUTO_CRUISE_CAP_US;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleMission() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    // Up to 32 waypoints * ~60 bytes/each + framing = ~2 KB worst case.
    DynamicJsonDocument req(3072);
    if (deserializeJson(req, server.arg("plain"))) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }
    JsonArray arr = req.as<JsonArray>();
    if (arr.isNull()) {
        server.send(400, "text/plain", "Body must be an array of {lat, lon}");
        return;
    }
    if (arr.size() == 0) {
        server.send(400, "text/plain", "Empty mission");
        return;
    }
    if (arr.size() > MAX_WAYPOINTS) {
        server.send(400, "application/json",
            "{\"ok\":false,\"err\":\"too many waypoints (max 32)\"}");
        return;
    }

    uint8_t i = 0;
    for (JsonObject wp : arr) {
        if (!wp.containsKey("lat") || !wp.containsKey("lon")) {
            server.send(400, "text/plain", "Each waypoint needs lat and lon");
            return;
        }
        waypoints[i].lat = wp["lat"].as<float>();
        waypoints[i].lon = wp["lon"].as<float>();
        i++;
    }
    wpCount       = i;
    wpIdx         = 0;
    missionActive = true;
    activeBearing = 0.0f;
    activeDistM   = 0.0f;
    Serial.printf("[MISSION] loaded %u waypoints, mission_active=true\n", wpCount);

    StaticJsonDocument<128> resp;
    resp["ok"]       = true;
    resp["wp_count"] = wpCount;
    resp["wp_idx"]   = wpIdx;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleMissionClear() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }
    wpCount       = 0;
    wpIdx         = 0;
    missionActive = false;
    Serial.println("[MISSION] cleared");
    server.send(200, "application/json", "{\"ok\":true,\"cleared\":true}");
}

static void handleSimGps() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }
    if (!req.containsKey("lat") || !req.containsKey("lon")) {
        server.send(400, "text/plain", "Need lat and lon");
        return;
    }
    boatLat      = req["lat"].as<float>();
    boatLon      = req["lon"].as<float>();
    gpsValid     = true;
    gpsSimulated = true;
    Serial.printf("[SIM_GPS] lat=%.6f lon=%.6f\n", boatLat, boatLon);

    StaticJsonDocument<128> resp;
    resp["ok"]            = true;
    resp["lat"]           = boatLat;
    resp["lon"]           = boatLon;
    resp["gps_simulated"] = gpsSimulated;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

// ── Mode state machine (same shape as test_27, no AUTO-side mission gate) ──
// AUTO is granted on (a) RC fresh, (b) guard clear, (c) no ack pending,
// (d) cruise valid. Mission state does NOT gate AUTO; instead applyOutputs()
// neutralizes when AUTO is engaged without a usable mission. Keeps the FSM
// orthogonal to the mission state.
static void updateMode() {
    static bool autoRefusalPrinted = false;
    Mode prev = mode;

    if (!swcInAuto() || cruiseUs >= AUTO_CRUISE_FLOOR_US) autoRefusalPrinted = false;

    if (!ibusEverGood) {
        mode = MODE_MANUAL;
        guardAboveSinceMs = 0;
        return;
    }

    if (ch[IBUS_IDX_FAILSAFE_GUARD] > FAILSAFE_GUARD_THRESHOLD) {
        if (guardAboveSinceMs == 0) guardAboveSinceMs = millis();
        if (millis() - guardAboveSinceMs >= FAILSAFE_DETECT_MS) {
            if (mode != MODE_FAILSAFE) {
                mode = MODE_FAILSAFE;
                failsafeAckRequired = true;
                ackRefusalPrinted   = false;
                // Snapshot mission state for gate 4 auto-detection.
                missionActiveAtFs = missionActive;
                wpIdxAtFs         = wpIdx;
                if (missionActive) fsDuringMission = true;
                Serial.printf("\n[FAILSAFE] guard tripped — ch[5]=%u sustained %lu ms. "
                              "Outputs neutral. Flip SwA UP (MANUAL) to ACK.\n",
                              ch[IBUS_IDX_FAILSAFE_GUARD],
                              millis() - guardAboveSinceMs);
            }
            return;
        }
    } else {
        guardAboveSinceMs = 0;
    }

    if (millis() - lastFrameMs >= FAILSAFE_NO_FRAME_MS) {
        if (mode != MODE_FAILSAFE) {
            mode = MODE_FAILSAFE;
            failsafeAckRequired = true;
            ackRefusalPrinted   = false;
            missionActiveAtFs = missionActive;
            wpIdxAtFs         = wpIdx;
            if (missionActive) fsDuringMission = true;
            Serial.printf("\n[FAILSAFE] no frames for %lu ms. "
                          "Outputs neutral. Flip SwA UP (MANUAL) to ACK after RC returns.\n",
                          millis() - lastFrameMs);
        }
        return;
    }

    if (failsafeAckRequired) {
        if (!ackRefusalPrinted) {
            Serial.println("[FAILSAFE] frames restored but ACK_REQUIRED — "
                           "flip SwA UP (MANUAL) to clear.");
            ackRefusalPrinted = true;
        }
        if (swcInManual()) {
            failsafeAckRequired = false;
            ackRefusalPrinted   = false;
            mode = MODE_MANUAL;
            Serial.println("[FAILSAFE] cleared (ACK via SwA=MANUAL). mode=MANUAL");
        }
        return;
    }

    Mode next = mode;
    if (swcInManual()) {
        next = MODE_MANUAL;
    } else if (swcInAuto()) {
        if (cruiseUs >= AUTO_CRUISE_FLOOR_US) {
            next = MODE_AUTO;
        } else {
            if (mode != MODE_MANUAL) next = MODE_MANUAL;
            if (!autoRefusalPrinted) {
                Serial.printf("[MODE] refusing AUTO: cruise=%u µs (need >= %u). "
                              "POST /cruise first.\n", cruiseUs, AUTO_CRUISE_FLOOR_US);
                autoRefusalPrinted = true;
            }
        }
    }
    mode = next;

    if (mode != prev) {
        if (mode == MODE_AUTO) {
            uint16_t engageUs = (cruiseUs > AUTO_CRUISE_CAP_US) ? AUTO_CRUISE_CAP_US : cruiseUs;
            if (missionActive && gpsValid && wpIdx < wpCount) {
                Serial.printf("[MODE] %s → AUTO (cruise=%u µs%s, wp_idx=%u/%u)\n",
                    modeName(prev), engageUs,
                    (cruiseUs > AUTO_CRUISE_CAP_US) ? " CAPPED" : "",
                    wpIdx, wpCount);
            } else {
                Serial.printf("[MODE] %s → AUTO (cruise=%u µs%s, NO MISSION — outputs neutral)\n",
                    modeName(prev), engageUs,
                    (cruiseUs > AUTO_CRUISE_CAP_US) ? " CAPPED" : "");
            }
        } else {
            Serial.printf("[MODE] %s → %s\n", modeName(prev), modeName(mode));
        }
    }
}

// ── Apply outputs ──────────────────────────────────────────────────────────
static void applyOutputs() {
    if (!ibusEverGood) {
        setRudder(NEUTRAL_US);
        setEscs(NEUTRAL_US);
        return;
    }
    switch (mode) {
      case MODE_MANUAL:
        setRudder(mapRudderStickToServo(ch[IBUS_IDX_RUDDER]));
        setEscs  (mapThrottleStickToEsc(ch[IBUS_IDX_THROTTLE]));
        break;
      case MODE_AUTO: {
        if (missionActive && gpsValid && wpIdx < wpCount) {
            uint16_t engageUs = (cruiseUs > AUTO_CRUISE_CAP_US) ? AUTO_CRUISE_CAP_US : cruiseUs;
            setEscs(engageUs);
            setRudder(headingHoldUs(activeBearing));
        } else {
            // AUTO with no mission or no fix — safe-hold.
            setRudder(NEUTRAL_US);
            setEscs(NEUTRAL_US);
        }
        break;
      }
      case MODE_FAILSAFE:
        setRudder(NEUTRAL_US);
        setEscs(NEUTRAL_US);
        break;
    }
}

// ── Phase prompts (each phase prints ONCE on entry) ────────────────────────
static void prompt() {
    switch (phase) {
      case P_WAIT_MISSION:
        Serial.println("STEP 1: POST a >=1 waypoint mission. Curl example:");
        Serial.printf( "  curl -X POST http://%s/mission -H 'Content-Type: application/json' \\\n",
                       boatIP.c_str());
        Serial.println("       -d '[{\"lat\":37.0001,\"lon\":-122.0001},"
                       "{\"lat\":37.0002,\"lon\":-122.0001},"
                       "{\"lat\":37.0002,\"lon\":-122.0002}]'");
        break;
      case P_WAIT_ADVANCE:
        Serial.println("STEP 2: POST /sim_gps far from WP1, then near WP1. Curl:");
        Serial.printf( "  curl -X POST http://%s/sim_gps -H 'Content-Type: application/json' "
                       "-d '{\"lat\":37.00,\"lon\":-122.00}'\n", boatIP.c_str());
        Serial.println("(any pair within ~3 m of waypoints[0] will trigger advance)");
        break;
      case P_WAIT_COMPLETE:
        Serial.println("STEP 3: POST /sim_gps within 3 m of each remaining waypoint in order.");
        Serial.println("(mission completes when wp_idx reaches wp_count)");
        break;
      case P_WAIT_FAILSAFE:
        Serial.println("STEP 4: POST a fresh mission, sim_gps far from WP1, /cruise, "
                       "flip SwA DOWN.");
        Serial.println("        Then KILL TX. After failsafe trips, restore TX, "
                       "SwA UP, then SwA DOWN.");
        Serial.println("(gate 4 passes when AUTO re-engages with mission_active still true "
                       "and wp_idx unchanged)");
        break;
      case P_DONE:
        break;
    }
}

static void printSummary() {
    Serial.println();
    Serial.println("========================================");
    Serial.println("  test_28 RESULTS");
    Serial.println("========================================");
    Serial.printf("Gate 1/4  Mission accepted             : %s\n", g1Pass ? "PASS" : "FAIL");
    Serial.printf("Gate 2/4  First waypoint advance       : %s\n", g2Pass ? "PASS" : "FAIL");
    Serial.printf("Gate 3/4  Mission completes cleanly    : %s\n", g3Pass ? "PASS" : "FAIL");
    Serial.printf("Gate 4/4  Mission survives FAILSAFE    : %s\n", g4Pass ? "PASS" : "FAIL");
    Serial.println("Outputs frozen at neutral. Reboot to re-run.");
}

static void runGates() {
    switch (phase) {
      case P_WAIT_MISSION:
        if (missionActive && wpCount >= 1) {
            g1Pass = true;
            Serial.printf("PASS (1/4): mission accepted, wp_count=%u, wp_idx=%u.\n",
                          wpCount, wpIdx);
            phase = P_WAIT_ADVANCE;
            prompt();
        }
        break;

      case P_WAIT_ADVANCE:
        if (wpIdx >= 1) {
            g2Pass = true;
            Serial.printf("PASS (2/4): first advance observed (wp_idx=%u).\n", wpIdx);
            // If the operator only loaded 1 waypoint, gate 3 already fired
            // implicitly (mission_active=false). Handle the cascade.
            if (!missionActive) {
                g3Pass = true;
                Serial.println("PASS (3/4): mission completed (single-waypoint mission).");
                phase = P_WAIT_FAILSAFE;
            } else {
                phase = P_WAIT_COMPLETE;
            }
            prompt();
        }
        break;

      case P_WAIT_COMPLETE:
        if (!missionActive) {
            // Mission just completed. If we're in AUTO at this moment, also
            // verify ESCs neutralized; otherwise the mode-governs-output
            // contract makes that check meaningless.
            if (mode == MODE_AUTO) {
                if (outPort == NEUTRAL_US) {
                    g3Pass = true;
                    Serial.println("PASS (3/4): mission complete, ESCs neutralized in AUTO.");
                } else {
                    g3Pass = false;
                    Serial.printf("FAIL (3/4): mission complete in AUTO but ESCs at %u µs "
                                  "(expected %u).\n", outPort, NEUTRAL_US);
                }
            } else {
                g3Pass = true;
                Serial.printf("PASS (3/4): mission complete (mode=%s, ESCs governed by mode).\n",
                              modeName(mode));
            }
            phase = P_WAIT_FAILSAFE;
            prompt();
        }
        break;

      case P_WAIT_FAILSAFE:
        // Gate 4 needs three things observed: (a) failsafe entered while a
        // mission was active, (b) mode is back to AUTO, (c) mission_active
        // and wp_idx survived the round-trip. The fsDuringMission latch
        // flips on the FAILSAFE-entry edge; missionActiveAtFs / wpIdxAtFs
        // capture the snapshot.
        if (fsDuringMission && missionActiveAtFs &&
            mode == MODE_AUTO && missionActive && wpIdx == wpIdxAtFs) {
            g4Pass = true;
            Serial.printf("PASS (4/4): mission survived FAILSAFE "
                          "(wp_idx=%u, mission_active=true).\n", wpIdx);
            phase = P_DONE;
            printSummary();
        }
        break;

      case P_DONE:
        break;
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("test_28_multi_waypoint  (PROPS OFF)");

    Wire.begin(21, 22);
    Wire.setClock(400000);

    pca.begin();
    pca.setOscillatorFrequency(27000000);
    pca.setPWMFreq(50);
    Wire.beginTransmission(0x40);
    bool pcaOK = (Wire.endTransmission() == 0);

    bool imuOK = false;
    for (int i = 0; i < 3; i++) {
        myICM.begin(Wire, 0);
        if (myICM.status == ICM_20948_Stat_Ok) { imuOK = true; break; }
        delay(500);
    }
    if (!pcaOK || !imuOK) {
        if (!pcaOK) Serial.println("FAIL: PCA9685 not at 0x40");
        if (!imuOK) Serial.println("FAIL: ICM-20948 not at 0x68");
        while (true) delay(1000);
    }

    ina219OK = ina219.begin();
    if (ina219OK) Serial.printf("[I2C] INA219 detected at 0x%02X\n", INA219_ADDR);
    else          Serial.printf("[I2C] WARN: INA219 not at 0x%02X — telemetry voltage disabled\n",
                                INA219_ADDR);

    setRudder(NEUTRAL_US); setEscs(NEUTRAL_US);
    Serial.println("Arming ESCs (3 s @ 1500 µs)...");
    delay(3000);

    ibusSerial.setRxBufferSize(1024);
    ibusSerial.begin(115200, SERIAL_8N1, IBUS_RX_PIN, -1);
    Serial.printf("iBUS on GPIO%d, mode=CH%d (idx %d).\n",
        IBUS_RX_PIN, IBUS_IDX_MODE + 1, IBUS_IDX_MODE);

    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("GPS on UART2 RX=GPIO%d TX=GPIO%d (sim_gps overrides once POSTed)\n",
        GPS_RX_PIN, GPS_TX_PIN);

    wifiSetup();

    server.on("/status",         HTTP_GET,     handleStatus);
    server.on("/status",         HTTP_OPTIONS, handleOptions);
    server.on("/telemetry",      HTTP_GET,     handleTelemetry);
    server.on("/telemetry",      HTTP_OPTIONS, handleOptions);
    server.on("/cruise",         HTTP_POST,    handleCruise);
    server.on("/cruise",         HTTP_OPTIONS, handleOptions);
    server.on("/mission",        HTTP_POST,    handleMission);
    server.on("/mission",        HTTP_OPTIONS, handleOptions);
    server.on("/mission/clear",  HTTP_POST,    handleMissionClear);
    server.on("/mission/clear",  HTTP_OPTIONS, handleOptions);
    server.on("/sim_gps",        HTTP_POST,    handleSimGps);
    server.on("/sim_gps",        HTTP_OPTIONS, handleOptions);
    server.begin();
    Serial.printf("HTTP up at http://%s/\n", boatIP.c_str());
    Serial.printf("Default cruise=%u µs. Capture radius=%.1f m. Max waypoints=%u.\n",
        DEFAULT_CRUISE_US, CAPTURE_RADIUS_M, MAX_WAYPOINTS);

    prompt();
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
    readIbus();
    updateImu();
    readIbus();
    pollIna219();
    updatePosition();
    server.handleClient();

    updateMode();
    updateSequencer();
    if (phase == P_DONE) {
        setRudder(NEUTRAL_US);
        setEscs(NEUTRAL_US);
    } else {
        applyOutputs();
    }
    runGates();
}
