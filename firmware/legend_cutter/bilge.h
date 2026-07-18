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

bool bilgeFwdWet();
bool bilgeMidWet();
bool bilgeRearWet();

bool         bilgePumpOn();          // current MOSFET state (HIGH during PHASE_ON only)
bool         bilgePumpManual();      // source == MANUAL
BilgePhase   bilgePumpPhase();
BilgeSource  bilgePumpSource();
uint8_t      bilgePumpCycle();       // 1-based count of ON pulses in current sequence
uint32_t     bilgePumpPhaseMs();     // millis since current phase began

void bilgeSetManual(bool on);
