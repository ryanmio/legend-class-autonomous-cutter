/*
 * legend_cutter.ino
 * Legend Class Autonomous Cutter — ESP32 firmware
 *
 * Direct port of tests/test_29_pool_integration (water-tested). The mode
 * FSM (MANUAL/AUTO/FAILSAFE) and the loop wiring live here; everything
 * else is owned by a single-purpose module.
 *
 * Mode FSM:
 *   - MANUAL    : rudder + ESCs follow CH1/CH2/CH3.
 *   - AUTO      : if waypoint+GPS valid and not captured, hold heading on
 *                 the haversine bearing with differential thrust; else
 *                 outputs neutral.
 *   - FAILSAFE  : SwD (CH6) sustained > 1500 µs for 500 ms, OR no iBUS
 *                 frames for 3 s. Outputs neutral. Cleared by flipping
 *                 SwA (CH7) to MANUAL position after RC returns.
 *
 * Safety: AUTO cruise cap 1750 µs; ESC hard floor MIN_REV_US / cap MAX_FWD_US
 * in motors.cpp. Captured waypoint freezes at neutral until operator
 * POSTs a new waypoint.
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

static bool swcInManual() { return ibusChannel(IBUS_IDX_MODE) < MODE_MAN_BELOW_US; }
static bool swcInAuto()   { return ibusChannel(IBUS_IDX_MODE) > MODE_AUTO_ABOVE_US; }

static void updateMode() {
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
}

static void applyOutputs() {
    if (!ibusEverGood()) {
        setRudder(NEUTRAL_US);
        setEscs(NEUTRAL_US);
        return;
    }
    switch (mode) {
      case MODE_MANUAL:
        setRudder(mapRudderStickToServo(ibusChannel(IBUS_IDX_RUDDER)));
        setEscs(computeThrottleUs(ibusChannel(IBUS_IDX_THROTTLE),
                                  ibusChannel(IBUS_IDX_REVERSE)));
        break;

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

    if (!motorsBegin()) { Serial.println("FAIL: PCA9685 not at 0x40"); while (true) delay(1000); }
    if (!imuBegin())    { Serial.println("FAIL: ICM-20948 not at 0x68"); while (true) delay(1000); }

    if (batteryBegin()) Serial.printf("[I2C] INA219 detected at 0x%02X\n", INA219_ADDR);
    else                Serial.printf("[I2C] WARN: INA219 not at 0x%02X — voltage telemetry disabled\n", INA219_ADDR);

    weaponsBegin();

    setRudder(NEUTRAL_US);
    setEscs(NEUTRAL_US);
    Serial.println("Arming ESCs (3 s @ 1500 µs)...");
    delay(3000);

    ibusBegin();
    gpsBegin();
    if (audioBegin()) Serial.println("[AUDIO] DF1201S ready.");
    else              Serial.println("[AUDIO] WARN: DF1201S did not ACK — /audio disabled.");

    telemetryBegin();

    Serial.printf("Default cruise=%u µs (cap %u). Default PID kp=%.2f kd=%.2f. Capture=%.1f m.\n",
        DEFAULT_CRUISE_US, AUTO_CRUISE_CAP_US, DEFAULT_KP, DEFAULT_KD, CAPTURE_RADIUS_M);
    Serial.println("Ready.");
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
    telemetryUpdate();

    // Compute + apply.
    updateMode();
    navUpdate();
    weaponsUpdate();
    applyOutputs();
}
