/*
 * legend_cutter.ino
 * Legend Class Autonomous Cutter — ESP32 firmware
 *
 * Faithful port of tests/test_29_pool_integration (the PASS'd
 * test_29-pool2.6-magcal2 build — first autonomous waypoint capture). The
 * mode FSM (MANUAL/AUTO/FAILSAFE) and the loop wiring live here; everything
 * else is owned by a single-purpose module.
 *
 * Mode FSM:
 *   - MANUAL    : rudder + ESCs follow CH1/CH2/CH3 (diff-thrust mixing).
 *   - AUTO      : if waypoint+GPS valid and not captured, hold the haversine
 *                 bearing on TRUE heading with differential thrust; else
 *                 outputs neutral. Cruise capped at AUTO_CRUISE_CAP_US.
 *   - FAILSAFE  : SwD (CH6) sustained > 1500 µs for 500 ms, OR no iBUS frames
 *                 for 3 s. Outputs neutral. Cleared by flipping SwA (CH7) to
 *                 MANUAL after RC returns.
 *
 * Safety: AUTO cruise cap = AUTO_CRUISE_CAP_US (1800 µs = MAX_FWD_US); ESC
 * hard floor/cap in motors.cpp. Captured waypoint freezes at neutral until
 * the operator POSTs a new waypoint.
 */

#include <Wire.h>
#include "config.h"
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
#include "telemetry.h"
#include "histlog.h"
#include "cmd.h"

enum VesselMode { MODE_MANUAL, MODE_AUTO, MODE_FAILSAFE };
static VesselMode mode                = MODE_MANUAL;
static bool       failsafeAckRequired = false;
static uint32_t   guardAboveSinceMs   = 0;

const char* vesselModeName() {
    switch (mode) {
      case MODE_MANUAL:   return "MANUAL";
      case MODE_AUTO:     return "AUTO";
      case MODE_FAILSAFE: return "FAILSAFE";
    }
    return "?";
}
bool vesselFailsafeAck() { return failsafeAckRequired; }

static bool swcInManual() { return ibusChannel(IBUS_IDX_MODE) < MODE_MAN_BELOW_US; }
static bool swcInAuto()   { return ibusChannel(IBUS_IDX_MODE) > MODE_AUTO_ABOVE_US; }

static const char* modeNameOf(VesselMode m) {
    switch (m) {
      case MODE_MANUAL:   return "MANUAL";
      case MODE_AUTO:     return "AUTO";
      case MODE_FAILSAFE: return "FAILSAFE";
    }
    return "?";
}

static void updateMode() {
    VesselMode prev = mode;

    if (!ibusEverGood()) {
        mode = MODE_MANUAL;
        guardAboveSinceMs = 0;
        return;
    }

    // SwD failsafe guard.
    uint16_t guard = ibusChannel(IBUS_IDX_FAILSAFE_GUARD);
    if (guard > FAILSAFE_GUARD_THRESHOLD) {
        if (guardAboveSinceMs == 0) guardAboveSinceMs = millis();
        if (millis() - guardAboveSinceMs >= FAILSAFE_DETECT_MS) {
            if (mode != MODE_FAILSAFE) {
                mode = MODE_FAILSAFE;
                failsafeAckRequired = true;
                Serial.println("[FAILSAFE] guard tripped — flip SwA UP (MANUAL) to ACK.");
            }
            return;
        }
    } else {
        guardAboveSinceMs = 0;
    }

    // No-frame failsafe.
    if (millis() - ibusLastFrameMs() >= FAILSAFE_NO_FRAME_MS) {
        if (mode != MODE_FAILSAFE) {
            mode = MODE_FAILSAFE;
            failsafeAckRequired = true;
            Serial.println("[FAILSAFE] no iBUS frames — flip SwA UP (MANUAL) to ACK after RC returns.");
        }
        return;
    }

    if (failsafeAckRequired) {
        if (swcInManual()) {
            failsafeAckRequired = false;
            mode = MODE_MANUAL;
            Serial.println("[FAILSAFE] cleared (ACK via SwA=MANUAL)");
        }
        return;
    }

    // SwA hysteresis: deadband holds current mode.
    if      (swcInManual()) mode = MODE_MANUAL;
    else if (swcInAuto())   mode = MODE_AUTO;

    if (mode == MODE_AUTO && prev != MODE_AUTO) {
        // Re-record the leg start at engage position — the crossing line must
        // be perpendicular to the path AUTO will actually drive.
        navResetLegStart();
    }
    if (mode != prev) {
        Serial.printf("[MODE] %s → %s\n", modeNameOf(prev), modeNameOf(mode));
    }
}

static void applyOutputs() {
    if (!ibusEverGood()) {
        setRudder(NEUTRAL_US);
        setEscs(NEUTRAL_US);
        return;
    }
    switch (mode) {
      case MODE_MANUAL: {
        uint16_t throttleUs = computeThrottleUs(ibusChannel(IBUS_IDX_THROTTLE),
                                                ibusChannel(IBUS_IDX_REVERSE));
        uint16_t rudderUs   = mapRudderStickToServo(ibusChannel(IBUS_IDX_RUDDER));
        uint16_t portUs, stbdUs;
        computePortStbd(throttleUs, rudderUs, portUs, stbdUs);
        setRudder(rudderUs);
        setEscsPortStbd(portUs, stbdUs);
        break;
      }

      case MODE_AUTO: {
        if (navWpSet() && gpsValid() && !navCaptured()) {
            uint16_t cruise   = telemetryCruiseUs();
            uint16_t engageUs = (cruise > AUTO_CRUISE_CAP_US) ? AUTO_CRUISE_CAP_US : cruise;
            uint16_t rudderUs = imuHeadingHoldUs(navWpBearing());
            uint16_t portUs, stbdUs;
            computePortStbd(engageUs, rudderUs, portUs, stbdUs);
            setRudder(rudderUs);
            setEscsPortStbd(portUs, stbdUs);
        } else {
            // No waypoint, no GPS, or already captured → safe-hold.
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

// Drain operator commands posted by the network task (core 0). Runs on the
// control loop (core 1) so all actuation stays on one core — no peripheral is
// ever touched from two cores, which is why no I2C/peripheral lock is needed.
static void cmdApply(const Command& c) {
    switch (c.type) {
      case CMD_WAYPOINT_SET:   { float d; navTrySetWaypoint(c.lat, c.lon, &d); break; }
      case CMD_WAYPOINT_CLEAR: navClearWaypoint();                             break;
      case CMD_CRUISE:         telemetrySetCruiseUs(c.cruiseUs);               break;
      case CMD_PID:            setPidGains(c.kp, c.kd);                        break;
      case CMD_LED:            lightsSet((LedId)c.ledId, c.ledOn);            break;
      case CMD_BILGE:          bilgeSetManual(c.bilgeOn);                      break;
      case CMD_RADAR:          radarSet(c.radarOn, c.radarSpeed, c.radarBurstMs, c.radarPauseMs); break;
      case CMD_DEPTH_MODE:     sonarSetMode(c.depthRun ? DEPTH_RUN : DEPTH_OFF); break;
      case CMD_DEPTH_PING:     sonarPingNow();                                 break;
      case CMD_MAGCAL_START:   imuMagCalBegin();                               break;
      case CMD_MAGCAL_ABORT:   imuMagCalAbort();                               break;
    }
}
static void cmdDrain() {
    Command c;
    while (cmdTryDequeue(c)) cmdApply(c);   // ≤ CMD_QUEUE_LEN cheap calls; never blocks
}

// Non-blocking bench serial console. The BOOT/EN button is sealed in the hull,
// so 'r' reboots over serial; 's' prints one status line (incl. network-task
// stack headroom) on demand — no streaming. Reads only buffered bytes; never
// blocks, so it doesn't touch the responsiveness guarantee.
static void serialConsole() {
    if (!Serial.available()) return;
    char c = Serial.read();
    if (c == 'r' || c == 'R') {
        Serial.println("[cmd] rebooting…");
        delay(50);                          // let the line flush
        ESP.restart();
    } else if (c == 's' || c == 'S') {
        Serial.printf("[status] v%s mode=%s up=%lus heap=%u net_stack_free=%u B\n",
            FIRMWARE_VERSION, vesselModeName(), (unsigned long)(millis() / 1000),
            ESP.getFreeHeap(), telemetryNetStackFreeBytes());
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("\n[BOOT] %s firmware v%s\n", VESSEL_NAME, FIRMWARE_VERSION);

    lightsBegin();
    bilgeBegin();
    radarBegin();
    sonarBegin();

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(I2C_FREQ_HZ);
    Wire.setTimeOut(I2C_TIMEOUT_MS);   // bound the bus — a stuck slave can't hang the loop

    // Neutralize outputs FIRST, before any init that could hang. The PCA9685 is
    // separately powered and latches its last PWM across an ESP reset, so after a
    // watchdog reboot the ESCs would otherwise hold cruise until reprogrammed.
    if (!motorsBegin()) Serial.println("WARN: PCA9685 not at 0x40 — outputs unavailable");
    setRudder(NEUTRAL_US);
    setEscs(NEUTRAL_US);

    // Degrade, don't hang: a missing IMU disables heading but MANUAL stays safe
    // and outputs are already neutral. (Was a permanent while(true) — which on a
    // reboot-into-fault left the ESCs latched at cruise forever.)
    if (!imuBegin()) Serial.println("WARN: ICM-20948 not at 0x68 — heading disabled; MANUAL still safe");

    if (batteryBegin()) Serial.printf("[I2C] INA219 detected at 0x%02X\n", INA219_ADDR);
    else                Serial.printf("[I2C] WARN: INA219 not at 0x%02X — voltage telemetry disabled\n", INA219_ADDR);

    weaponsBegin();

    Serial.println("Arming ESCs (3 s @ 1500 µs)...");
    delay(3000);

    ibusBegin();
    gpsBegin();
    if (audioBegin()) Serial.println("[AUDIO] DF1201S ready.");
    else              Serial.println("[AUDIO] WARN: DF1201S did not ACK — /audio disabled.");

    telemetryBegin();
    histlogBegin();

    Serial.printf("Default cruise=%u µs (cap %u). Default PID kp=%.2f kd=%.2f. Capture=%.1f m.\n",
        DEFAULT_CRUISE_US, AUTO_CRUISE_CAP_US, DEFAULT_KP, DEFAULT_KD, CAPTURE_RADIUS_M);
    Serial.println("Ready.");

    // Backstop only — with networking off the control loop this should never
    // fire. If an unforeseen block ever wedges the loop, the task WDT reboots;
    // setup() then neutralizes outputs first and the RAM-only waypoint is gone,
    // so the boat comes up MANUAL/neutral and never resumes AUTO. Enabled last
    // so the long wifiConnect() in setup can't trip it.
    enableLoopWDT();
}

void loop() {
    // Read inputs.
    ibusUpdate();
    imuUpdate();
    ibusUpdate();              // second poll: low-latency for SwD detection
    batteryUpdate();
    bilgeUpdate();
    radarUpdate();
    sonarUpdate();
    gpsUpdate();
    imuUpdateCogTrim();
    cmdDrain();                // apply operator commands queued by the core-0 network task
    histlogUpdate();           // rate-limited capture into the RAM history ring

    // Compute + apply.
    updateMode();
    navUpdate(mode == MODE_AUTO);
    weaponsUpdate();           // CH5 knob → deck-gun pan servo (independent of mode)
    applyOutputs();

    serialConsole();           // non-blocking bench console: 'r' reboot, 's' status
    feedLoopWDT();             // backstop heartbeat (never starves in normal operation)
}
