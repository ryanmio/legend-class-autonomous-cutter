// bilge.cpp
// Auto-pump while the mid sensor is wet, with a dry-delay tail. Manual
// override forces the pump until the timeout. Run-dry protection latches
// auto-pump off after BILGE_MAX_RUN_MS continuous on; sensors must
// actually go dry once before auto re-engages (manual still works as an
// escape hatch).
//
// fwd/rear probes are informational only — they appear in telemetry but
// do not drive the pump.

#include "bilge.h"
#include "config.h"

static bool     fwdWet = false, midWet = false, rearWet = false;
static bool     pumpOn = false, pumpManual = false;
static uint32_t lastWetMs     = 0;
static uint32_t manualUntilMs = 0;
static uint32_t pumpOnSinceMs = 0;
static bool     stuckLatch    = false;

void bilgeBegin() {
    pinMode(PIN_BILGE_FWD_SENSOR,  INPUT_PULLUP);
    pinMode(PIN_BILGE_MID_SENSOR,  INPUT_PULLUP);
    pinMode(PIN_BILGE_REAR_SENSOR, INPUT_PULLUP);
    pinMode(PIN_BILGE_PUMP, OUTPUT);
    digitalWrite(PIN_BILGE_PUMP, LOW);
}

void bilgeUpdate() {
    uint32_t now = millis();
    fwdWet  = (digitalRead(PIN_BILGE_FWD_SENSOR)  == LOW);
    midWet  = (digitalRead(PIN_BILGE_MID_SENSOR)  == LOW);
    rearWet = (digitalRead(PIN_BILGE_REAR_SENSOR) == LOW);

    // Only mid drives the pump.
    bool anyWet = midWet;
    if (anyWet) lastWetMs = now;
    if (!anyWet && stuckLatch) stuckLatch = false;

    if (pumpManual && (int32_t)(now - manualUntilMs) >= 0) {
        pumpManual = false;
    }

    bool autoOn = !stuckLatch &&
                  (anyWet || (lastWetMs != 0 && (now - lastWetMs) < BILGE_DRY_DELAY_MS));
    bool wantOn = pumpManual || autoOn;

    // Run-dry cutoff — auto-pump only, manual is exempt.
    if (pumpOn && !pumpManual && (now - pumpOnSinceMs) >= BILGE_MAX_RUN_MS) {
        stuckLatch = true;
        wantOn = false;
    }

    if (wantOn != pumpOn) {
        pumpOn = wantOn;
        if (pumpOn) pumpOnSinceMs = now;
        digitalWrite(PIN_BILGE_PUMP, pumpOn ? HIGH : LOW);
    }
}

bool bilgeFwdWet()      { return fwdWet; }
bool bilgeMidWet()      { return midWet; }
bool bilgeRearWet()     { return rearWet; }
bool bilgePumpOn()      { return pumpOn; }
bool bilgePumpManual()  { return pumpManual; }
bool bilgeStuck()       { return stuckLatch; }

void bilgeSetManual(bool on) {
    if (on) {
        pumpManual    = true;
        manualUntilMs = millis() + BILGE_MANUAL_TIMEOUT_MS;
    } else {
        pumpManual = false;
    }
}
