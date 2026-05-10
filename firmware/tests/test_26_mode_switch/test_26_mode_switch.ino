/*
 * test_26_mode_switch.ino - v4
 *
 * !!!  STALE CHANNEL MAP — DO NOT COPY INTO NEW CODE  !!!
 * As of 2026-05-10 the iBUS channel layout changed to support a real RC
 * failsafe (test_27). This sketch's `IBUS_IDX_MODE = 5` (CH6) is now the
 * SwD FAILSAFE GUARD; the mode switch (SwA) moved to CH7 (idx 6).
 * Current canonical map: see firmware/legend_cutter/config.h and the
 * iBUS section of handoff prompt.md. This sketch is preserved as a
 * historical PASS record only — do NOT lift its channel constants
 * for production firmware or future tests.
 *
 * What's NEW vs everything before:
 *   - SwC (CH6, iBUS index 5) selects MANUAL vs AUTO.
 *     UP (~1000 µs) = MANUAL, DOWN (~2000 µs) = AUTO.
 *     Inverted from v3 so that the TX-forced switch-up boot state = MANUAL.
 *   - In AUTO, the FIRMWARE commands the throttle (boat moves on its own
 *     for the first time ever in this project).
 *   - AUTO cruise µs is CAPTURED from the operator's peak throttle stick
 *     during the MANUAL passthrough phase and mapped through the same
 *     stick-to-ESC function used in MANUAL. A floor (1650 µs) refuses
 *     AUTO if the operator never demonstrably revved past the deadband;
 *     a ceiling (1750 µs ≈ 50% forward) caps full-stick captures so the
 *     bench rig can't bolt at 60% with no water resistance.
 *   - After the AUTO spool the sketch asks the operator "did both motors
 *     spin? y/n" — gate 2 PASSes only on 'y'. The firmware can't measure
 *     spin directly, so the operator is the source of truth.
 *   - Mode transitions are safe — flipping mid-run doesn't jump or runaway.
 *
 * What this sketch does NOT re-test (already proven, don't re-walk):
 *   - Manual rudder/throttle pass-through, diff thrust, reverse interlock
 *     (test_17). MANUAL gate here is a single-frame sanity check, not a sweep.
 *   - Heading hold rudder geometry (test_25). AUTO gate proves the THROTTLE
 *     side; rudder is along for the ride.
 *
 * Gate flow (auto-detected, sketch prompts):
 *   1/3  SwC UP, sticks safe → mode reads MANUAL.
 *   2/3  Drive manually with sticks; peak throttle stick value is captured.
 *        Return throttle to idle and flip SwC DOWN → firmware ramps both
 *        ESCs to the captured cruise µs for AUTO_TEST_MS, then returns to
 *        neutral. Heading hold runs in parallel (target = heading at AUTO
 *        entry). Sketch then prompts: "Did both motors spin? Type y or n."
 *   3/3  Operator flips SwC UP → mode returns to MANUAL, outputs frozen
 *        at neutral, summary printed. Sketch is done.
 *
 * No LIVE / free-play mode. After gate 3, outputs stay neutral until reboot.
 *
 * Failsafe: if iBUS frames stop or freeze at any point, outputs go neutral
 * and the sketch prints a FAIL line for the active gate.
 *
 * Hardware (additions to test_25):
 *   Receiver iBUS → 1kΩ → GPIO 16 (with 2kΩ to GND)
 *   Port ESC      → PCA9685 ch0
 *   Stbd ESC      → PCA9685 ch1
 *   Rudder        → PCA9685 ch2 (already wired)
 *   ICM-20948     → I2C @ 0x68 (already wired)
 *
 * SAFETY:
 *   PROPS OFF or boat firmly secured. AUTO cruise is capped at 1750 µs
 *   (≈50% forward) and never exceeds MAX_FWD_US=1800. AUTO_TEST_MS = 1000 ms.
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include "ICM_20948.h"
#include <math.h>

// ── PCA9685 channels ────────────────────────────────────────────────────────
static const uint8_t  CH_ESC_PORT = 0;
static const uint8_t  CH_ESC_STBD = 1;
static const uint8_t  CH_RUDDER   = 2;

// ── Output bounds ───────────────────────────────────────────────────────────
static const uint16_t RUDDER_MIN_US = 1330;     // test_15
static const uint16_t RUDDER_MAX_US = 1670;
static const uint16_t NEUTRAL_US    = 1500;

// ── ESC/throttle limits (match test_17) ─────────────────────────────────────
static const uint16_t MAX_FWD_US     = 1800;    // 60% forward cap on ESC output

// ── AUTO-throttle cruise selection ──────────────────────────────────────────
// AUTO cruise µs is captured from the operator's peak throttle stick during
// MANUAL passthrough, then mapped through mapThrottleStickToEsc() (the same
// mapping used in MANUAL). Floor + cap guard:
//   - FLOOR: refuses AUTO if mapped peak < FLOOR. Operator never demonstrably
//     revved past the ESC deadband, so we have no idea whether the bench
//     ESCs will spin at any value. 1650 µs sits above the 1600-µs deadband
//     observed on this rig (1600 commanded, motors did not spin, 2026-05-10).
//   - CAP:   ceiling on cruise even if operator pushes full stick. 1750 µs
//     ≈ 50% forward — well within the existing MAX_FWD_US=1800 hard clamp,
//     but lower because AUTO is hands-off and we don't want the boat to
//     bolt at 60% on a no-load bench rig.
static const uint16_t AUTO_CRUISE_FLOOR_US = 1650;
static const uint16_t AUTO_CRUISE_CAP_US   = 1750;
static const uint32_t AUTO_TEST_MS         = 1000;    // 1 second spool

// ── iBUS channel indices ────────────────────────────────────────────────────
static const uint8_t  IBUS_IDX_RUDDER   = 0;
static const uint8_t  IBUS_IDX_THROTTLE = 2;
static const uint8_t  IBUS_IDX_MODE     = 5;
static const uint8_t  IBUS_RX_PIN       = 16;

// ── Mode hysteresis (SwC up ≈ 1000 = MANUAL, down ≈ 2000 = AUTO) ────────────
// Inverted from earlier draft: the FS-i6X forces SwC UP on TX power-up, so
// MANUAL must be the up position. AUTO is the deliberate down-flick.
static const uint16_t MODE_MAN_BELOW_US  = 1450;
static const uint16_t MODE_AUTO_ABOVE_US = 1550;

// ── Stick safe-arm thresholds ───────────────────────────────────────────────
static const uint16_t THROTTLE_IDLE_MAX = 1100;
static const uint16_t STICK_NEUTRAL_DB  = 30;

// ── Failsafe ────────────────────────────────────────────────────────────────
// 1500/2000 leaves headroom over the test_15/16 baseline iBUS noise (~1%, bursty).
// The real fix for catastrophic loss seen in early test_26 runs was upstream:
// IMU getAGMT() was starving the iBUS UART buffer (see IMU throttling below).
static const uint32_t FAILSAFE_MS       = 1500;
static const uint32_t FROZEN_TIMEOUT_MS = 2000;
static const uint32_t HINT_INTERVAL_MS  = 10000;

// IMU update throttle — 50 Hz is plenty for heading hold and bounds the time
// the loop spends in I2C, leaving the iBUS UART time to drain.
static const uint32_t IMU_UPDATE_INTERVAL_MS = 20;

// ── Heading-hold gains (used in AUTO gate only) ────────────────────────────
static const float Kp = 3.0f;
static const float Kd = 8.0f;

// ── Mag offsets (test_22 outdoor) ──────────────────────────────────────────
static const float MAG_OFFSET_X = -20.70f;
static const float MAG_OFFSET_Y =  -0.45f;
static const float MAG_OFFSET_Z = -17.70f;
static const float ALPHA = 0.98f;

// ── Hardware ───────────────────────────────────────────────────────────────
static Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
static ICM_20948_I2C myICM;
static HardwareSerial ibusSerial(1);

// ── Gate sequence ──────────────────────────────────────────────────────────
enum Gate { G_WAIT_MANUAL, G_WAIT_AUTO, G_AUTO_RUN, G_WAIT_AUTO_CONFIRM,
            G_WAIT_MAN_RETURN, G_DONE };
static Gate gate = G_WAIT_MANUAL;

// ── State ──────────────────────────────────────────────────────────────────
static uint8_t  ibusBuf[32], ibusIdx = 0;
static uint16_t ch[10] = {0};
static bool     ibusEverGood = false;
static uint32_t lastFrameMs = 0, lastChangeMs = 0;
static uint32_t lastHintMs = 0;
static uint32_t lastChecksumWarnMs = 0;

static uint16_t outRudder = NEUTRAL_US, outPort = NEUTRAL_US, outStbd = NEUTRAL_US;
static float    fusedHeading = 0, prevHeadingForD = 0;
static uint32_t lastDtUs = 0;
static unsigned long lastImuUs = 0;
static bool     headingInit = false;

static float    autoTargetHeading = 0;
static uint32_t autoStartMs       = 0;
static float    autoMaxDriftDeg   = 0;
static int      autoMaxRudDefl    = 0;

// Captured during MANUAL passthrough; latched into autoCruiseUs at AUTO entry.
static uint16_t maxManualThrottleStickUs = 0;
static uint16_t autoCruiseUs             = 0;

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
    // Hard safety clamp — under no circumstances should ESCs see anything
    // outside [NEUTRAL_US, MAX_FWD_US]. Reverse is disabled.
    if (us < NEUTRAL_US) us = NEUTRAL_US;
    if (us > MAX_FWD_US) us = MAX_FWD_US;
    outPort = outStbd = us;
    writePCA(CH_ESC_PORT, us);
    writePCA(CH_ESC_STBD, us);
}

// Map throttle stick (rests at BOTTOM ~1000 µs) to ESC output. Matches
// test_17's computeThrottleUs(): stick at idle = motors stopped (1500),
// stick lifted scales linearly to MAX_FWD_US.
static uint16_t mapThrottleStickToEsc(uint16_t stickUs) {
    if (stickUs <= THROTTLE_IDLE_MAX) return NEUTRAL_US;
    if (stickUs >= 2000) return MAX_FWD_US;
    return (uint16_t)map(stickUs, THROTTLE_IDLE_MAX, 2000, NEUTRAL_US, MAX_FWD_US);
}

// Map rudder stick (spring-centered at 1500 µs) to rudder servo output.
// Matches test_17's scaledRudder(): full stick range 1000..2000 maps linearly
// to RUDDER_MIN_US..RUDDER_MAX_US.
static uint16_t mapRudderStickToServo(uint16_t stickUs) {
    if (stickUs < 1000) stickUs = 1000;
    if (stickUs > 2000) stickUs = 2000;
    if (stickUs <= 1500) return (uint16_t)map(stickUs, 1000, 1500, RUDDER_MIN_US, NEUTRAL_US);
    return                       (uint16_t)map(stickUs, 1500, 2000, NEUTRAL_US, RUDDER_MAX_US);
}
static void neutralAll() { setRudder(NEUTRAL_US); setEscs(NEUTRAL_US); }

static float shortestPathError(float t, float c) {
    float e = t - c;
    while (e >  180.0f) e -= 360.0f;
    while (e < -180.0f) e += 360.0f;
    return e;
}

static bool framesOK() {
    // Frame-presence check only. The "channels frozen for N ms" check was
    // removed 2026-05-10 after freeze_diag confirmed the FS-i6X / FS-iA10B
    // produces no gimbal jitter on idle sticks — so any operator pause
    // longer than the threshold trips a false failsafe. On a boat that
    // legitimately runs straight for long stretches, that detector is
    // worse than useless. Real receiver loss = no frames at all = caught
    // by the FAILSAFE_MS timeout above.
    return ibusEverGood && (millis() - lastFrameMs < FAILSAFE_MS);
}
static bool throttleAtIdle() { return ch[IBUS_IDX_THROTTLE] <= THROTTLE_IDLE_MAX; }
static bool rudderCentered() { return abs((int)ch[IBUS_IDX_RUDDER] - NEUTRAL_US) <= STICK_NEUTRAL_DB; }
static bool swcInManual()    { return ch[IBUS_IDX_MODE] < MODE_MAN_BELOW_US; }
static bool swcInAuto()      { return ch[IBUS_IDX_MODE] > MODE_AUTO_ABOVE_US; }

// ── iBUS parsing ───────────────────────────────────────────────────────────
static bool parseIbus() {
    if (ibusBuf[0] != 0x20 || ibusBuf[1] != 0x40) return false;
    uint16_t sum = 0xFFFF;
    for (int i = 0; i < 30; i++) sum -= ibusBuf[i];
    uint16_t rx = ibusBuf[30] | (ibusBuf[31] << 8);
    if (sum != rx) {
        // Bad checksum — silently drop the frame. ~1% of frames per
        // test_15/test_16 baseline; printing them is noise.
        return false;
    }
    bool changed = false;
    for (int i = 0; i < 10; i++) {
        uint16_t v = ibusBuf[2 + i*2] | (ibusBuf[3 + i*2] << 8);
        if (v != ch[i]) changed = true;
        ch[i] = v;
    }
    lastFrameMs = millis();
    if (changed) lastChangeMs = lastFrameMs;
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

// Drain Serial input down to the latest non-whitespace char. Used by the
// G_WAIT_AUTO_CONFIRM gate to consume the operator's y/n keystroke. Same
// pattern as freeze_diag/readCommand().
static char readSerialChar() {
    char last = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c > ' ') last = c;
    }
    return last;
}

// ── IMU + heading-hold (only used during AUTO gate) ────────────────────────
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

// ── Gate prompts (printed once on entry, repeated every 10 s while waiting) ─
static void prompt() {
    switch (gate) {
      case G_WAIT_MANUAL:
        Serial.println("  ...turn TX on, throttle BOTTOM, sticks centered, SwC UP.");
        break;
      case G_WAIT_AUTO:
        Serial.println("  ...drive manually. The peak throttle you reach here BECOMES the AUTO");
        Serial.println("     cruise speed (capped at 1750 µs). Push at least to mid-stick so the");
        Serial.println("     captured value clears the ESC deadband.");
        Serial.println("     When ready, return throttle to idle and flip SwC DOWN for AUTO test.");
        break;
      case G_WAIT_MAN_RETURN:
        Serial.println("  ...flip SwC UP to finish.");
        break;
      case G_AUTO_RUN:
      case G_WAIT_AUTO_CONFIRM:
      case G_DONE:
        break;
    }
}

// ── Final summary ──────────────────────────────────────────────────────────
static void printSummary(bool g1, bool g2, bool g3) {
    Serial.println();
    Serial.println("========================================");
    Serial.println("  test_26 RESULTS");
    Serial.println("========================================");
    Serial.printf("Gate 1/3  MANUAL detected           : %s\n", g1 ? "PASS" : "FAIL");
    Serial.printf("Gate 2/3  AUTO autonomous throttle  : %s\n", g2 ? "PASS" : "FAIL");
    Serial.printf("Gate 3/3  AUTO->MANUAL flip safe    : %s\n", g3 ? "PASS" : "FAIL");
    if (g2) {
        Serial.printf("AUTO test: cruise=%u µs for %lu ms, max heading drift %.1f°, max rudder defl %d µs\n",
            autoCruiseUs, AUTO_TEST_MS, autoMaxDriftDeg, autoMaxRudDefl);
    }
    Serial.println("Outputs frozen at neutral. Reboot to re-run.");
}

// ── Main step ──────────────────────────────────────────────────────────────
static bool g1Pass = false, g2Pass = false, g3Pass = false;

static void step() {
    // Failsafe always wins.
    if (!framesOK()) {
        neutralAll();
        if (gate != G_WAIT_MANUAL && gate != G_DONE) {
            Serial.println();
            Serial.println("FAIL: iBUS lost mid-test. Outputs neutral. Reboot to retry.");
            printSummary(g1Pass, g2Pass, g3Pass);
            gate = G_DONE;
        }
        return;
    }

    switch (gate) {
      case G_WAIT_MANUAL:
        neutralAll();
        if (throttleAtIdle() && rudderCentered() && swcInManual()) {
            g1Pass = true;
            Serial.printf("PASS (1/3): MANUAL detected (CH6=%u µs).\n", ch[IBUS_IDX_MODE]);
            gate = G_WAIT_AUTO;
            lastHintMs = millis();
            prompt();
        }
        return;

      case G_WAIT_AUTO: {
        // MANUAL passthrough while waiting for SwC to flip down. Operator can
        // verify rudder + ESCs from sticks, just like test_17. Stick values
        // go through mapThrottleStickToEsc() and mapRudderStickToServo() —
        // bottom-rest throttle conventions, NOT raw stick µs.
        // We also track the operator's peak throttle stick value so the AUTO
        // gate can run at a cruise µs we know spins this rig's motors.
        setRudder(mapRudderStickToServo(ch[IBUS_IDX_RUDDER]));
        setEscs  (mapThrottleStickToEsc(ch[IBUS_IDX_THROTTLE]));
        if (ch[IBUS_IDX_THROTTLE] > maxManualThrottleStickUs)
            maxManualThrottleStickUs = ch[IBUS_IDX_THROTTLE];
        // AUTO only triggers when SwC is in the AUTO position AND throttle
        // is back to idle (so we never enter AUTO mid-rev).
        if (swcInAuto() && throttleAtIdle()) {
            uint16_t mappedPeak = mapThrottleStickToEsc(maxManualThrottleStickUs);
            if (mappedPeak < AUTO_CRUISE_FLOOR_US) {
                neutralAll();
                Serial.printf("FAIL (2/3): manual peak only mapped to %u µs (need >= %u). "
                              "Rev throttle in MANUAL before flipping to AUTO.\n",
                              mappedPeak, AUTO_CRUISE_FLOOR_US);
                gate = G_WAIT_MAN_RETURN;
                lastHintMs = millis();
                prompt();
                return;
            }
            autoCruiseUs = (mappedPeak > AUTO_CRUISE_CAP_US)
                           ? AUTO_CRUISE_CAP_US : mappedPeak;
            Serial.printf("AUTO entering: cruise=%u µs (manual peak stick=%u µs, mapped=%u µs).\n",
                autoCruiseUs, maxManualThrottleStickUs, mappedPeak);
            autoTargetHeading = fusedHeading;
            autoStartMs       = millis();
            autoMaxDriftDeg   = 0;
            autoMaxRudDefl    = 0;
            gate = G_AUTO_RUN;
        }
        return;
      }

      case G_AUTO_RUN: {
        // Operator aborted by flipping SwC back early.
        if (!swcInAuto()) {
            neutralAll();
            Serial.println("FAIL (2/3): SwC flipped back before AUTO test completed.");
            gate = G_WAIT_MAN_RETURN;
            return;
        }
        // Drive ESCs to captured cruise, rudder via heading hold.
        setEscs(autoCruiseUs);
        uint16_t rud = headingHoldUs(autoTargetHeading);
        setRudder(rud);
        // Track max drift / rudder deflection for the summary.
        float drift = fabsf(shortestPathError(autoTargetHeading, fusedHeading));
        if (drift > autoMaxDriftDeg) autoMaxDriftDeg = drift;
        int defl = (int)rud - (int)NEUTRAL_US;
        if (abs(defl) > abs(autoMaxRudDefl)) autoMaxRudDefl = defl;
        // Time's up — drop ESCs and hand off to operator confirm.
        if (millis() - autoStartMs >= AUTO_TEST_MS) {
            setEscs(NEUTRAL_US);
            setRudder(NEUTRAL_US);
            Serial.println();
            Serial.println("Did BOTH motors spin during AUTO? Type y or n.");
            (void)readSerialChar();   // drain any stray input from before the prompt
            gate = G_WAIT_AUTO_CONFIRM;
        }
        return;
      }

      case G_WAIT_AUTO_CONFIRM: {
        // Outputs frozen at neutral while we wait for the operator's verdict.
        // Failsafe at top of step() still applies — if iBUS dies during the
        // wait, gate goes to G_DONE with FAIL.
        setEscs(NEUTRAL_US);
        setRudder(NEUTRAL_US);
        char c = readSerialChar();
        if (c == 'y' || c == 'Y') {
            g2Pass = true;
            Serial.printf("PASS (2/3): operator confirmed motors spun. cruise=%u µs for %lu ms, "
                          "max heading drift %.1f°, max rudder defl %d µs.\n",
                          autoCruiseUs, AUTO_TEST_MS, autoMaxDriftDeg, autoMaxRudDefl);
            gate = G_WAIT_MAN_RETURN;
            lastHintMs = millis();
            prompt();
        } else if (c == 'n' || c == 'N') {
            g2Pass = false;
            Serial.printf("FAIL (2/3): operator reported motors did NOT spin at cruise=%u µs.\n",
                          autoCruiseUs);
            gate = G_WAIT_MAN_RETURN;
            lastHintMs = millis();
            prompt();
        }
        return;
      }

      case G_WAIT_MAN_RETURN:
        neutralAll();
        if (swcInManual()) {
            g3Pass = true;
            Serial.printf("PASS (3/3): SwC->MANUAL, outputs neutral (CH6=%u µs).\n",
                ch[IBUS_IDX_MODE]);
            printSummary(g1Pass, g2Pass, g3Pass);
            gate = G_DONE;
        }
        return;

      case G_DONE:
        neutralAll();
        return;
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("test_26_mode_switch  (PROPS OFF)");

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

    neutralAll();
    Serial.println("Arming ESCs (3 s @ 1500 µs)...");
    delay(3000);

    // Larger UART RX buffer — 1024 bytes ≈ 230 ms of iBUS data. Default 256
    // overflows when updateImu() stalls the loop, which destroys frame sync.
    ibusSerial.setRxBufferSize(1024);
    ibusSerial.begin(115200, SERIAL_8N1, IBUS_RX_PIN, -1);
    Serial.printf("Ready. iBUS on GPIO%d, mode=CH%d (idx %d).\n",
        IBUS_RX_PIN, IBUS_IDX_MODE + 1, IBUS_IDX_MODE);
    prompt();
    lastHintMs = millis();
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
    readIbus();      // drain UART before any I2C work
    updateImu();
    readIbus();      // drain again in case I2C took a while
    step();
    if (gate != G_DONE && gate != G_AUTO_RUN && gate != G_WAIT_AUTO_CONFIRM &&
        millis() - lastHintMs > HINT_INTERVAL_MS) {
        prompt();
        lastHintMs = millis();
    }
}
