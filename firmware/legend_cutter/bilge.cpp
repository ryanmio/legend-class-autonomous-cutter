// bilge.cpp
// Duty-cycled bilge pump driven by the rear probe (only).
//
// AUTO — bursts, then rests, forever (never gives up: a full bilge is the
// normal steady state, not a fault):
//   OFF      --rear wet--> ON (cycle=1)
//   ON       --PULSE_ON elapsed--> PAUSE
//   PAUSE    --PULSE_OFF elapsed--> ON (cycle++) while cycle < BURST_CYCLES,
//                                   else COOLDOWN
//   COOLDOWN --COOLDOWN_MS elapsed--> ON (cycle=1) if rear still wet, else OFF
// The burst is committed once started (rides out probe flicker, primes the
// pump, lets water settle); rear is only re-checked at the cooldown boundary.
//
// MANUAL — operator-engaged, cycles ON/PAUSE forever (no burst cap, no
// cooldown) until bilgeSetManual(false) stops it immediately.

#include "bilge.h"
#include "config.h"

static bool fwdWet = false, midWet = false, rearWet = false;

static BilgePhase  phase    = BILGE_PHASE_OFF;
static BilgeSource source   = BILGE_SRC_NONE;
static uint8_t     cycleNum = 0;
static uint32_t    phaseStartMs = 0;

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

    // MANUAL: operator override — cycle ON/PAUSE forever, no cap, no cooldown.
    if (source == BILGE_SRC_MANUAL) {
        uint32_t elapsed = now - phaseStartMs;
        if      (phase == BILGE_PHASE_ON    && elapsed >= BILGE_PULSE_ON_MS)  enterPhase(BILGE_PHASE_PAUSE, now);
        else if (phase == BILGE_PHASE_PAUSE && elapsed >= BILGE_PULSE_OFF_MS) { cycleNum++; enterPhase(BILGE_PHASE_ON, now); }
        return;
    }

    // AUTO burst/cooldown state machine.
    uint32_t elapsed = now - phaseStartMs;
    switch (phase) {
      case BILGE_PHASE_OFF:
        if (rearWet) { source = BILGE_SRC_AUTO; cycleNum = 1; enterPhase(BILGE_PHASE_ON, now); }
        break;

      case BILGE_PHASE_ON:
        if (elapsed >= BILGE_PULSE_ON_MS) enterPhase(BILGE_PHASE_PAUSE, now);
        break;

      case BILGE_PHASE_PAUSE:
        if (elapsed >= BILGE_PULSE_OFF_MS) {
            if (cycleNum < BILGE_BURST_CYCLES) { cycleNum++; enterPhase(BILGE_PHASE_ON, now); }
            else                                enterPhase(BILGE_PHASE_COOLDOWN, now);
        }
        break;

      case BILGE_PHASE_COOLDOWN:
        if (elapsed >= BILGE_COOLDOWN_MS) {
            if (rearWet) { cycleNum = 1; enterPhase(BILGE_PHASE_ON, now); }   // still wet → another burst
            else         stopAll();                                          // dry → idle until it wets again
        }
        break;
    }
}

bool bilgeFwdWet()      { return fwdWet; }
bool bilgeMidWet()      { return midWet; }
bool bilgeRearWet()     { return rearWet; }
bool bilgePumpOn()      { return phase == BILGE_PHASE_ON; }
bool bilgePumpManual()  { return source == BILGE_SRC_MANUAL; }

BilgePhase  bilgePumpPhase()    { return phase; }
BilgeSource bilgePumpSource()   { return source; }
uint8_t     bilgePumpCycle()    { return cycleNum; }
uint32_t    bilgePumpPhaseMs()  { return phase == BILGE_PHASE_OFF ? 0 : (millis() - phaseStartMs); }

void bilgeSetManual(bool on) {
    uint32_t now = millis();
    if (on) {
        source   = BILGE_SRC_MANUAL;
        cycleNum = 1;
        enterPhase(BILGE_PHASE_ON, now);
    } else {
        stopAll();
    }
}
