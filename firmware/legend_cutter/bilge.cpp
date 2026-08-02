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

// Raw reads are written on core 1 and read cross-core by the HTTP handlers on
// core 0, hence volatile (aligned bool/uint32 loads are atomic on this target).
static volatile bool fwdWet = false, midWet = false, rearWet = false;

// Derived probe views, indexed 0=fwd 1=mid 2=rear — see bilge.h for the
// latched/hits/duty contract. Control consumers keep the raw reads.
static uint32_t          latchUntilMs[3] = {0, 0, 0};
static volatile bool     latched[3]      = {false, false, false};
static volatile uint32_t hits[3]         = {0, 0, 0};
static volatile uint8_t  dutyPct[3]      = {0, 0, 0};
static uint32_t          lowReads[3]     = {0, 0, 0};
static uint32_t          totalReads      = 0;
static uint32_t          dutyWindowMs    = 0;

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
    dutyWindowMs = millis();
}

// Latch stretch, hit counting, and low-duty accumulation for one probe.
static void probeDerive(uint8_t i, bool wet, uint32_t now) {
    if (wet) {
        latchUntilMs[i] = now + BILGE_LATCH_MS;
        lowReads[i]++;
    }
    bool l = wet || (int32_t)(latchUntilMs[i] - now) > 0;
    if (l && !latched[i]) hits[i] = hits[i] + 1;
    latched[i] = l;
}

void bilgeUpdate() {
    uint32_t now = millis();
    fwdWet  = (digitalRead(PIN_BILGE_FWD_SENSOR)  == LOW);
    midWet  = (digitalRead(PIN_BILGE_MID_SENSOR)  == LOW);
    rearWet = (digitalRead(PIN_BILGE_REAR_SENSOR) == LOW);

    probeDerive(0, fwdWet, now);
    probeDerive(1, midWet, now);
    probeDerive(2, rearWet, now);
    totalReads++;
    if (now - dutyWindowMs >= BILGE_DUTY_WINDOW_MS) {
        for (uint8_t i = 0; i < 3; i++) {
            dutyPct[i]  = (uint8_t)((100u * lowReads[i] + totalReads / 2) / totalReads);
            lowReads[i] = 0;
        }
        totalReads   = 0;
        dutyWindowMs = now;
    }

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

bool bilgeFwdWetLatched()  { return latched[0]; }
bool bilgeMidWetLatched()  { return latched[1]; }
bool bilgeRearWetLatched() { return latched[2]; }

uint32_t bilgeFwdHits()  { return hits[0]; }
uint32_t bilgeMidHits()  { return hits[1]; }
uint32_t bilgeRearHits() { return hits[2]; }

uint8_t bilgeFwdDutyPct()  { return dutyPct[0]; }
uint8_t bilgeMidDutyPct()  { return dutyPct[1]; }
uint8_t bilgeRearDutyPct() { return dutyPct[2]; }
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
