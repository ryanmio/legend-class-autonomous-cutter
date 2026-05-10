/*
 * test_27_rc_failsafe.ino - v2 (failsafe-guard)
 *
 * v1 (no-frame timeout) failed live: FS-iA10B holds-last on TX-off, frames
 * keep streaming, no-frame timeout never fires. v2 detects RC loss via a
 * dedicated guard channel (CH6 / idx 5) configured on the TX failsafe menu.
 * SwD sits up (~1000 µs) in normal use; receiver outputs ~2000 µs from its
 * stored failsafe value when the TX is gone. Detector: ch[5] > 1500
 * sustained 500 ms → MODE_FAILSAFE. The 3 s no-frame timeout stays as
 * defense-in-depth for receivers that genuinely go silent.
 *
 * Channel map (locked 2026-05-10 — see handoff prompt):
 *   CH1 (idx 0)  rudder           CH5 (idx 4)  knob (gun pan / winch)
 *   CH2 (idx 1)  reverse          CH6 (idx 5)  SwD failsafe guard
 *   CH3 (idx 2)  throttle         CH7 (idx 6)  SwA mode (up=MANUAL,
 *   CH4 (idx 3)  unused                          down=AUTO)
 *
 * What's NEW vs test_26:
 *   - Canonical Mode enum (MODE_MANUAL / MODE_AUTO / MODE_FAILSAFE) — same
 *     shape the production firmware will inherit.
 *   - Failsafe-guard detection on CH6 (idx 5) — the actual TX-loss signal
 *     for FS-iA10B with this TX/RX configuration.
 *   - 3 s no-frame timeout as defense-in-depth alongside the guard.
 *   - STICKY ACK: once FAILSAFE trips, firmware refuses to re-engage AUTO
 *     even after RC frames return — operator must flip SwA UP (MANUAL) to
 *     acknowledge before AUTO can be re-armed. Prevents the runaway case
 *     where TX glitches mid-mission and recovers with the switch still in
 *     AUTO.
 *   - Cruise µs is set via HTTP `POST /cruise` (default 1660 — quiet, just
 *     above the ESC deadband). Same flow the app will use in water.
 *   - HTTP `GET /telemetry` exposes mode, cruise, RC age, guard µs, INA219
 *     voltage, fused heading. Single biggest force-multiplier for the
 *     first water test — operator sees voltage without unscrewing the
 *     hatch.
 *
 * What this sketch does NOT prove (already covered, don't re-walk):
 *   - Manual stick → ESC / rudder mapping (test_17). MANUAL passthrough is
 *     used here only as a side-effect of the mode state machine.
 *   - SwA mode classifier hysteresis (test_26). Reused as-is.
 *   - Heading-hold rudder geometry (test_25). AUTO uses heading hold so the
 *     failsafe has something running to interrupt; the rudder behavior is
 *     not under test.
 *
 * Gate flow (auto-detected, sketch prompts):
 *   1/4  Boot, SwA UP, sticks safe → MODE_MANUAL detected.
 *   2/4  Operator POSTs /cruise via HTTP, then flips SwA DOWN → AUTO with
 *        motors at cruise µs. Operator confirms motors spinning.
 *   3/4  Operator turns TX off. Within 3 s the no-frame timeout trips,
 *        mode → FAILSAFE, outputs go neutral. Operator confirms motors
 *        stopped (y/n).
 *   4/4  Operator turns TX back on with SwA still in AUTO position. Sketch
 *        observes ACK_REQUIRED (frames good but mode stays FAILSAFE).
 *        Operator flips SwA UP → ack accepted, mode → MANUAL. Operator
 *        flips SwA DOWN → AUTO re-engages, motors spin again.
 *
 * Failsafe applies regardless of pre-failure mode. If TX dies in MANUAL,
 * mode still goes to FAILSAFE; the ack is just trivially satisfied because
 * SwA is already in MANUAL when frames return.
 *
 * Hardware (additions to test_26):
 *   INA219            → I2C @ 0x41 (high-side current sense, already wired)
 *   WiFi              → connects to home SSID, falls back to hotspot, then
 *                       to a "LegendCutter"/coastguard AP.
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
// Cruise µs is set via HTTP `POST /cruise`. AUTO refuses to engage until a
// value at or above the floor has been received. Same floor / cap reasoning
// as test_26: floor sits above this rig's observed deadband (1600 µs failed
// to spin, 1653 µs spun reliably), cap stays well below MAX_FWD_US so a
// hands-off AUTO can't bolt at 60% on a no-load bench.
static const uint16_t AUTO_CRUISE_FLOOR_US = 1650;
static const uint16_t AUTO_CRUISE_CAP_US   = 1750;

// ── iBUS channel indices (locked 2026-05-10) ────────────────────────────────
static const uint8_t  IBUS_IDX_RUDDER         = 0;   // CH1
static const uint8_t  IBUS_IDX_THROTTLE       = 2;   // CH3
static const uint8_t  IBUS_IDX_FAILSAFE_GUARD = 5;   // CH6 — SwD
static const uint8_t  IBUS_IDX_MODE           = 6;   // CH7 — SwA
static const uint8_t  IBUS_RX_PIN             = 16;

// ── SwA hysteresis (up = MANUAL, down = AUTO; same as test_26) ──────────────
static const uint16_t MODE_MAN_BELOW_US  = 1450;
static const uint16_t MODE_AUTO_ABOVE_US = 1550;

// ── Stick safe-arm ──────────────────────────────────────────────────────────
static const uint16_t THROTTLE_IDLE_MAX = 1100;
static const uint16_t STICK_NEUTRAL_DB  = 30;

// ── Failsafe ────────────────────────────────────────────────────────────────
// PRIMARY: dedicated guard channel (CH6 / idx 5 / SwD). FS-iA10B holds-last
// on TX-off so iBUS frames keep arriving; the receiver overrides only the
// configured-failsafe channel (CH6) with the stored value (~2000 µs) when
// the TX is gone. Threshold + 500 ms debounce ignores deliberate flicks.
static const uint16_t FAILSAFE_GUARD_THRESHOLD = 1500;
static const uint32_t FAILSAFE_DETECT_MS       = 500;
// SECONDARY (defense-in-depth): no-frame timeout. Catches receivers that
// genuinely go silent on signal loss — not what FS-iA10B does, but the
// production firmware should be portable across receivers.
static const uint32_t FAILSAFE_NO_FRAME_MS     = 3000;

// ── Default cruise (operator can override via POST /cruise) ─────────────────
// 1660 µs is just above test_26's confirmed-spin point of 1653 µs. The
// motors at 1700 µs were violently loud on the bench and risked vibrating
// the boat structure (Ryan, 2026-05-10). Slow it down; POST /cruise can
// raise it for in-water testing where there's drag to load the motors.
static const uint16_t DEFAULT_CRUISE_US = 1660;

// ── IMU update throttle ────────────────────────────────────────────────────
static const uint32_t IMU_UPDATE_INTERVAL_MS = 20;

// ── Heading-hold gains (used during AUTO; rudder behavior not under test) ──
static const float Kp = 3.0f;
static const float Kd = 8.0f;

// ── Mag offsets (test_22 outdoor; copied from test_26) ──────────────────────
static const float MAG_OFFSET_X = -20.70f;
static const float MAG_OFFSET_Y =  -0.45f;
static const float MAG_OFFSET_Z = -17.70f;
static const float ALPHA = 0.98f;

// ── INA219 ──────────────────────────────────────────────────────────────────
static const uint8_t INA219_ADDR = 0x41;

// ── Hardware ───────────────────────────────────────────────────────────────
static Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
static Adafruit_INA219          ina219(INA219_ADDR);
static ICM_20948_I2C            myICM;
static HardwareSerial           ibusSerial(1);
static WebServer                server(80);
static String                   boatIP;
static bool                     ina219OK = false;

// ── Mode state machine ─────────────────────────────────────────────────────
enum Mode { MODE_MANUAL, MODE_AUTO, MODE_FAILSAFE };
static Mode    mode                  = MODE_MANUAL;
static bool    failsafeAckRequired   = false;
static bool    ackRefusalPrinted     = false;
static uint16_t cruiseUs             = DEFAULT_CRUISE_US;

// ── Test phase (drives gate prompts) ────────────────────────────────────────
enum Phase {
    P_BOOT,                 // waiting for clean MANUAL boot → gate 1
    P_AUTO_FIRST,           // observed gate 1; waiting for /cruise + AUTO,
                            // then the failsafe trip
    P_FAILSAFE_CONFIRM,     // failsafe tripped; awaiting y/n
    P_RECOVERY,             // awaiting TX-on, ack, AUTO re-engage
    P_RECOVERY_CONFIRM,     // AUTO re-engaged; awaiting y/n
    P_DONE
};
static Phase phase = P_BOOT;
static bool  g1Pass = false, g2Pass = false, g3Pass = false, g4Pass = false;

// Phase scratch state.
static uint32_t firstAutoEnteredMs        = 0;
static bool     firstAutoEnteredPrinted   = false;
static bool     failsafeReportedThisPhase = false;
static uint32_t recoveryAutoEngagedMs     = 0;

// ── iBUS / RC state ─────────────────────────────────────────────────────────
static uint8_t  ibusBuf[32], ibusIdx = 0;
static uint16_t ch[10]   = {0};
static bool     ibusEverGood     = false;
static uint32_t lastFrameMs      = 0;
static uint32_t guardAboveSinceMs = 0;   // 0 = guard not currently above threshold

// ── Output state ────────────────────────────────────────────────────────────
static uint16_t outRudder = NEUTRAL_US, outPort = NEUTRAL_US, outStbd = NEUTRAL_US;

// ── IMU / heading hold state ────────────────────────────────────────────────
static float    fusedHeading       = 0;
static float    prevHeadingForD    = 0;
static uint32_t lastDtUs           = 0;
static unsigned long lastImuUs     = 0;
static bool     headingInit        = false;
static float    autoTargetHeading  = 0;

// ── INA219 cache (don't poll on every loop) ─────────────────────────────────
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
    // Hard safety clamp — under no circumstances do ESCs see anything outside
    // [NEUTRAL_US, MAX_FWD_US]. Reverse is disabled.
    if (us < NEUTRAL_US) us = NEUTRAL_US;
    if (us > MAX_FWD_US) us = MAX_FWD_US;
    outPort = outStbd = us;
    writePCA(CH_ESC_PORT, us);
    writePCA(CH_ESC_STBD, us);
}

// Stick → output mapping (same shape as test_17 / test_26).
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

// Drain serial input down to the latest non-whitespace char (y/n).
static char readSerialChar() {
    char last = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c > ' ') last = c;
    }
    return last;
}

// ── IMU + heading hold (used in AUTO; rudder behavior not under test) ───────
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

// ── INA219 polling ─────────────────────────────────────────────────────────
static void pollIna219() {
    if (!ina219OK) return;
    if (millis() - lastInaPollMs < INA_POLL_INTERVAL_MS) return;
    lastInaPollMs = millis();
    busVoltage = ina219.getBusVoltage_V() + (ina219.getShuntVoltage_mV() / 1000.0f);
    shuntMa    = ina219.getCurrent_mA();
}

// ── WiFi ────────────────────────────────────────────────────────────────────
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
        "{\"ok\":true,\"v\":\"test_27\",\"ip\":\"" + boatIP + "\"}");
}

static void handleTelemetry() {
    addCORS();
    StaticJsonDocument<512> doc;
    doc["v"]                = "test_27";
    doc["uptime"]           = millis() / 1000;
    doc["heap"]             = ESP.getFreeHeap();
    doc["mode"]             = modeName(mode);
    doc["cruise_us"]        = cruiseUs;
    doc["failsafe_ack"]     = failsafeAckRequired;
    doc["rc_ever_good"]     = ibusEverGood;
    doc["rc_age_ms"]        = ibusEverGood ? (millis() - lastFrameMs) : 0;
    doc["rudder_us"]        = outRudder;
    doc["esc_us"]           = outPort;
    doc["ch_throttle"]      = ch[IBUS_IDX_THROTTLE];
    doc["ch_rudder"]        = ch[IBUS_IDX_RUDDER];
    doc["ch_mode"]          = ch[IBUS_IDX_MODE];
    doc["ch_guard"]         = ch[IBUS_IDX_FAILSAFE_GUARD];

    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f", fusedHeading); doc["heading"] = buf;
    if (ina219OK) {
        snprintf(buf, sizeof(buf), "%.2f", busVoltage); doc["bus_v"]    = buf;
        snprintf(buf, sizeof(buf), "%.0f", shuntMa);    doc["shunt_ma"] = buf;
    }

    char out[512];
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

// ── Mode state machine ─────────────────────────────────────────────────────
static void updateMode() {
    static bool autoRefusalPrinted = false;
    Mode prev = mode;

    // Reset the "refusing AUTO" latch whenever the conditions for refusal no
    // longer hold (SwA away from AUTO, or cruise has been set above floor).
    // Done up here so the reset fires regardless of which branch we take.
    if (!swcInAuto() || cruiseUs >= AUTO_CRUISE_FLOOR_US) autoRefusalPrinted = false;

    // Pre-RC safety: never call this an RC failure if we never had RC at all.
    // Outputs are forced neutral by applyOutputs() while !ibusEverGood.
    if (!ibusEverGood) {
        mode = MODE_MANUAL;
        guardAboveSinceMs = 0;
        return;
    }

    // PRIMARY failsafe: dedicated guard channel (ch[5] / SwD). Receiver outputs
    // its stored failsafe value (~2000 µs) on TX-loss; debounce 500 ms to ignore
    // any brief electrical blip.
    if (ch[IBUS_IDX_FAILSAFE_GUARD] > FAILSAFE_GUARD_THRESHOLD) {
        if (guardAboveSinceMs == 0) guardAboveSinceMs = millis();
        if (millis() - guardAboveSinceMs >= FAILSAFE_DETECT_MS) {
            if (mode != MODE_FAILSAFE) {
                mode = MODE_FAILSAFE;
                failsafeAckRequired = true;
                ackRefusalPrinted   = false;
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

    // SECONDARY failsafe (defense-in-depth): no frames for 3 s. Catches receivers
    // that genuinely go silent on signal loss — not what FS-iA10B does today, but
    // a future receiver swap or genuine wiring failure should still trip safe.
    if (millis() - lastFrameMs >= FAILSAFE_NO_FRAME_MS) {
        if (mode != MODE_FAILSAFE) {
            mode = MODE_FAILSAFE;
            failsafeAckRequired = true;
            ackRefusalPrinted   = false;
            Serial.printf("\n[FAILSAFE] no frames for %lu ms. "
                          "Outputs neutral. Flip SwA UP (MANUAL) to ACK after RC returns.\n",
                          millis() - lastFrameMs);
        }
        return;
    }

    // Frames are fresh and guard is clear. Sticky ack: stay in FAILSAFE until we
    // observe SwA=MANUAL even if the guard already cleared.
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

    // Normal classification.
    Mode next = mode;
    if (swcInManual()) {
        next = MODE_MANUAL;
    } else if (swcInAuto()) {
        if (cruiseUs >= AUTO_CRUISE_FLOOR_US) {
            if (mode != MODE_AUTO) {
                // First entry into AUTO this cycle — latch heading target.
                autoTargetHeading = fusedHeading;
            }
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
    // SwA in dead-band (1450..1550) → keep current mode (hysteresis).
    mode = next;

    if (mode != prev) {
        if (mode == MODE_AUTO) {
            uint16_t engageUs = (cruiseUs > AUTO_CRUISE_CAP_US) ? AUTO_CRUISE_CAP_US : cruiseUs;
            Serial.printf("[MODE] %s → %s (cruise=%u µs%s, target heading=%.1f°)\n",
                modeName(prev), modeName(mode), engageUs,
                (cruiseUs > AUTO_CRUISE_CAP_US) ? " CAPPED" : "",
                autoTargetHeading);
        } else {
            Serial.printf("[MODE] %s → %s\n", modeName(prev), modeName(mode));
        }
    }
}

// ── Apply outputs based on mode ────────────────────────────────────────────
static void applyOutputs() {
    if (!ibusEverGood) {
        // Pre-RC safe state — outputs neutral regardless of mode value.
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
        uint16_t engageUs = (cruiseUs > AUTO_CRUISE_CAP_US) ? AUTO_CRUISE_CAP_US : cruiseUs;
        setEscs(engageUs);
        setRudder(headingHoldUs(autoTargetHeading));
        break;
      }
      case MODE_FAILSAFE:
        setRudder(NEUTRAL_US);
        setEscs(NEUTRAL_US);
        break;
    }
}

// ── Phase-driven gate prompts (each phase prints ONCE on entry) ─────────────
static void prompt() {
    switch (phase) {
      case P_BOOT:
        Serial.println("STEP 1: TX on, sticks safe, SwA UP.");
        break;
      case P_AUTO_FIRST:
        Serial.printf("STEP 2: flip SwA DOWN to engage AUTO at %u us.\n", cruiseUs);
        break;
      case P_RECOVERY:
        Serial.println("STEP 3: TX back on with SwA=AUTO. Then flip SwA UP, then DOWN.");
        break;
      case P_FAILSAFE_CONFIRM:
      case P_RECOVERY_CONFIRM:
      case P_DONE:
        break;
    }
}

static void printSummary() {
    Serial.println();
    Serial.println("========================================");
    Serial.println("  test_27 RESULTS");
    Serial.println("========================================");
    Serial.printf("Gate 1/4  MANUAL detected at boot      : %s\n", g1Pass ? "PASS" : "FAIL");
    Serial.printf("Gate 2/4  /cruise + AUTO engages       : %s\n", g2Pass ? "PASS" : "FAIL");
    Serial.printf("Gate 3/4  3 s RC loss → FAILSAFE       : %s\n", g3Pass ? "PASS" : "FAIL");
    Serial.printf("Gate 4/4  ACK + AUTO re-engage         : %s\n", g4Pass ? "PASS" : "FAIL");
    Serial.println("Outputs frozen at neutral. Reboot to re-run.");
}

static void runGates() {
    switch (phase) {
      case P_BOOT:
        if (mode == MODE_MANUAL && ibusEverGood &&
            throttleAtIdle() && rudderCentered() && swcInManual()) {
            g1Pass = true;
            Serial.printf("PASS (1/4): MANUAL detected.\n");
            phase = P_AUTO_FIRST;
            prompt();
        }
        break;

      case P_AUTO_FIRST:
        if (mode == MODE_AUTO && !firstAutoEnteredPrinted) {
            firstAutoEnteredPrinted = true;
            firstAutoEnteredMs      = millis();
            Serial.println("AUTO engaged. Verify motors spinning, then turn off the TX.");
        }
        if (mode == MODE_FAILSAFE && !failsafeReportedThisPhase) {
            failsafeReportedThisPhase = true;
            Serial.println("Did motors spin in AUTO and then STOP after TX off? y/n");
            (void)readSerialChar();
            phase = P_FAILSAFE_CONFIRM;
        }
        break;

      case P_FAILSAFE_CONFIRM: {
        char c = readSerialChar();
        if (c == 'y' || c == 'Y') {
            g2Pass = firstAutoEnteredPrinted;
            g3Pass = true;
            if (g2Pass) Serial.println("PASS (2/4): AUTO engaged.");
            else        Serial.println("FAIL (2/4): AUTO never engaged — re-run and flip SwA DOWN before TX off.");
            Serial.println("PASS (3/4): RC loss neutralized outputs.");
            phase = P_RECOVERY;
            prompt();
        } else if (c == 'n' || c == 'N') {
            g3Pass = false;
            Serial.println("FAIL (3/4): operator reports motors did not spin or did not stop.");
            phase = P_DONE;
            printSummary();
        }
        break;
      }

      case P_RECOVERY:
        if (mode == MODE_AUTO) {
            recoveryAutoEngagedMs = millis();
            Serial.println("AUTO re-engaged. Motors spinning again? y/n");
            (void)readSerialChar();
            phase = P_RECOVERY_CONFIRM;
        }
        break;

      case P_RECOVERY_CONFIRM: {
        char c = readSerialChar();
        if (c == 'y' || c == 'Y') {
            g4Pass = true;
            Serial.println("PASS (4/4): full recovery (ack + re-engage).");
            phase = P_DONE;
            printSummary();
        } else if (c == 'n' || c == 'N') {
            g4Pass = false;
            Serial.println("FAIL (4/4): motors did not spin after recovery.");
            phase = P_DONE;
            printSummary();
        }
        break;
      }

      case P_DONE:
        // Outputs are forced neutral by the post-DONE branch in loop().
        break;
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("test_27_rc_failsafe  (PROPS OFF)");

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

    // INA219 is best-effort — telemetry omits voltage if it's missing,
    // but the test still runs.
    ina219OK = ina219.begin();
    if (ina219OK) {
        Serial.printf("[I2C] INA219 detected at 0x%02X\n", INA219_ADDR);
    } else {
        Serial.printf("[I2C] WARN: INA219 not found at 0x%02X — telemetry voltage disabled\n",
            INA219_ADDR);
    }

    setRudder(NEUTRAL_US); setEscs(NEUTRAL_US);
    Serial.println("Arming ESCs (3 s @ 1500 µs)...");
    delay(3000);

    // Larger UART RX buffer (matches test_26 — default 256 overflows when
    // I2C work stalls the loop).
    ibusSerial.setRxBufferSize(1024);
    ibusSerial.begin(115200, SERIAL_8N1, IBUS_RX_PIN, -1);
    Serial.printf("iBUS on GPIO%d, mode=CH%d (idx %d).\n",
        IBUS_RX_PIN, IBUS_IDX_MODE + 1, IBUS_IDX_MODE);

    wifiSetup();

    server.on("/status",    HTTP_GET,     handleStatus);
    server.on("/status",    HTTP_OPTIONS, handleOptions);
    server.on("/telemetry", HTTP_GET,     handleTelemetry);
    server.on("/telemetry", HTTP_OPTIONS, handleOptions);
    server.on("/cruise",    HTTP_POST,    handleCruise);
    server.on("/cruise",    HTTP_OPTIONS, handleOptions);
    server.begin();
    Serial.printf("HTTP up at http://%s/  (POST /cruise to override; GET /telemetry)\n",
        boatIP.c_str());
    Serial.printf("Default cruise=%u us. Failsafe=%lu ms.\n",
        DEFAULT_CRUISE_US, FAILSAFE_NO_FRAME_MS);

    prompt();
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
    readIbus();
    updateImu();
    readIbus();
    pollIna219();
    server.handleClient();

    updateMode();
    if (phase == P_DONE) {
        // After the test concludes, hold outputs at neutral regardless of mode.
        setRudder(NEUTRAL_US);
        setEscs(NEUTRAL_US);
    } else {
        applyOutputs();
    }
    runGates();
}
