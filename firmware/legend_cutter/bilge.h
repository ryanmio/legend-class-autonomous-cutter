// bilge.h
// Pump lives in the REAR compartment (moved 2026-05-27). Only the rear
// probe drives the pump; fwd/mid are informational.
//
// Pulse: BILGE_PULSE_ON_MS on, BILGE_PULSE_OFF_MS off.
//   AUTO   — on rear-wet, runs BILGE_BURST_CYCLES pulses then a
//            BILGE_COOLDOWN_MS rest, and repeats forever (never gives up).
//            The burst is committed once started; rear is re-checked only at
//            the end of each cooldown (still wet → burst again, dry → idle).
//   MANUAL — operator-engaged, cycles forever until the operator stops.

#pragma once

#include <Arduino.h>

enum BilgePhase  { BILGE_PHASE_OFF = 0, BILGE_PHASE_ON = 1, BILGE_PHASE_PAUSE = 2, BILGE_PHASE_COOLDOWN = 3 };
enum BilgeSource { BILGE_SRC_NONE  = 0, BILGE_SRC_AUTO = 1, BILGE_SRC_MANUAL = 2 };

void bilgeBegin();
void bilgeUpdate();

// Raw per-loop probe reads — the control-consumer contract (pump FSM, flood
// alarm). A marginal probe can read low for less than a loop pass, so 1 Hz
// consumers must use the latched views below, never these.
bool bilgeFwdWet();
bool bilgeMidWet();
bool bilgeRearWet();

// Derived views for telemetry and the flight log:
//   latched — any raw low stretched to BILGE_LATCH_MS so a 1 Hz sampler sees it
//   hits    — count of latch rising edges since boot (sampling-rate immune)
//   duty    — % of loop reads low over the last BILGE_DUTY_WINDOW_MS: contact
//             quality (clean submerged ≈ 100, marginal ≈ single digits, 0 = dry)
bool     bilgeFwdWetLatched();
bool     bilgeMidWetLatched();
bool     bilgeRearWetLatched();
uint32_t bilgeFwdHits();
uint32_t bilgeMidHits();
uint32_t bilgeRearHits();
uint8_t  bilgeFwdDutyPct();
uint8_t  bilgeMidDutyPct();
uint8_t  bilgeRearDutyPct();

bool         bilgePumpOn();          // current MOSFET state (HIGH during PHASE_ON only)
bool         bilgePumpManual();      // source == MANUAL
BilgePhase   bilgePumpPhase();
BilgeSource  bilgePumpSource();
uint8_t      bilgePumpCycle();       // 1-based count of ON pulses in current sequence
uint32_t     bilgePumpPhaseMs();     // millis since current phase began

void bilgeSetManual(bool on);
