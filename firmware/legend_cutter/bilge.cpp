// bilge.cpp
// Duty-cycled bilge pump driven by the rear probe (only). Phase transitions:
//
//   OFF  --(rear wet & !stuck) OR (manual on)--> ON  (cycle=1, sequenceStart=now)
//   ON   --PULSE_ON elapsed--> PAUSE
//   PAUSE --PULSE_OFF elapsed:
//             AUTO + still wet + (now-sequenceStart) < AUTO_MAX_MS  --> ON  (cycle++)
//             AUTO + dry                                            --> OFF
//             AUTO + still wet + cap reached                        --> OFF + stuck latch
//             MANUAL                                                --> ON  (cycle++)
//
// Operator stop (bilgeSetManual(false)) → OFF immediately.
// A real dry reading clears the stuck latch (auto can re-engage next time).

#include "bilge.h"
#include "config.h"

static bool fwdWet = false, midWet = false, rearWet = false;

static BilgePhase  phase    = BILGE_PHASE_OFF;
static BilgeSource source   = BILGE_SRC_NONE;
static uint8_t     cycleNum = 0;
static uint32_t    phaseStartMs    = 0;
static uint32_t    sequenceStartMs = 0;
static bool        stuckLatch      = false;

static void writePump(bool on) {
    digitalWrite(PIN_BILGE_PUMP, on ? HIGH : LOW);
}

static void enterPhase(BilgePhase p, uint32_t now) {
    phase = p;
    phaseStartMs = now;
    writePump(p == BILGE_PHASE_ON);
}

static void stopAll() {
    phase    = BILGE_PHASE_OFF;
    source   = BILGE_SRC_NONE;
    cycleNum = 0;
    writePump(false);
}

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

    // Real dry reading clears the stuck latch so auto can re-engage later.
    if (!rearWet && stuckLatch) stuckLatch = false;

    // Pick the source we want to be in right now.
    BilgeSource want;
    if (source == BILGE_SRC_MANUAL)        want = BILGE_SRC_MANUAL;
    else if (rearWet && !stuckLatch)       want = BILGE_SRC_AUTO;
    else                                   want = BILGE_SRC_NONE;

    if (want == BILGE_SRC_NONE) {
        if (phase != BILGE_PHASE_OFF) stopAll();
        return;
    }

    // Starting a new sequence (transition from OFF, or source switching).
    if (source != want) {
        source          = want;
        cycleNum        = 1;
        sequenceStartMs = now;
        enterPhase(BILGE_PHASE_ON, now);
        return;
    }

    // Mid-sequence: step the phase timer.
    uint32_t phaseElapsed = now - phaseStartMs;

    if (phase == BILGE_PHASE_ON && phaseElapsed >= BILGE_PULSE_ON_MS) {
        enterPhase(BILGE_PHASE_PAUSE, now);
        return;
    }

    if (phase == BILGE_PHASE_PAUSE && phaseElapsed >= BILGE_PULSE_OFF_MS) {
        if (source == BILGE_SRC_AUTO) {
            if (!rearWet) { stopAll(); return; }
            if ((now - sequenceStartMs) >= BILGE_AUTO_MAX_MS) {
                stuckLatch = true;
                stopAll();
                return;
            }
        }
        // MANUAL has no cap; AUTO continues.
        cycleNum++;
        enterPhase(BILGE_PHASE_ON, now);
    }
}

bool bilgeFwdWet()      { return fwdWet; }
bool bilgeMidWet()      { return midWet; }
bool bilgeRearWet()     { return rearWet; }
bool bilgePumpOn()      { return phase == BILGE_PHASE_ON; }
bool bilgePumpManual()  { return source == BILGE_SRC_MANUAL; }
bool bilgeStuck()       { return stuckLatch; }

BilgePhase  bilgePumpPhase()    { return phase; }
BilgeSource bilgePumpSource()   { return source; }
uint8_t     bilgePumpCycle()    { return cycleNum; }
uint32_t    bilgePumpPhaseMs()  { return phase == BILGE_PHASE_OFF ? 0 : (millis() - phaseStartMs); }

void bilgeSetManual(bool on) {
    uint32_t now = millis();
    if (on) {
        source          = BILGE_SRC_MANUAL;
        cycleNum        = 1;
        sequenceStartMs = now;
        stuckLatch      = false;   // operator override
        enterPhase(BILGE_PHASE_ON, now);
    } else {
        stopAll();
    }
}
