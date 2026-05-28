// bilge.h
// Pump lives in the REAR compartment (moved 2026-05-27). Only the rear
// probe drives the pump; fwd/mid are informational.
//
// Duty cycle: BILGE_PULSE_ON_MS on, BILGE_PULSE_OFF_MS off, repeat.
//   AUTO   — engages on rear-wet, caps at BILGE_AUTO_MAX_MS total elapsed;
//            if rear is still wet at the cap, sets stuck latch and
//            requires the operator to engage manually.
//   MANUAL — operator-engaged, cycles forever until the operator stops.

#pragma once

#include <Arduino.h>

enum BilgePhase  { BILGE_PHASE_OFF = 0, BILGE_PHASE_ON = 1, BILGE_PHASE_PAUSE = 2 };
enum BilgeSource { BILGE_SRC_NONE  = 0, BILGE_SRC_AUTO = 1, BILGE_SRC_MANUAL = 2 };

void bilgeBegin();
void bilgeUpdate();

bool bilgeFwdWet();
bool bilgeMidWet();
bool bilgeRearWet();

bool         bilgePumpOn();          // current MOSFET state (HIGH during PHASE_ON only)
bool         bilgePumpManual();      // source == MANUAL
bool         bilgeStuck();           // auto exhausted at AUTO_MAX_MS while still wet
BilgePhase   bilgePumpPhase();
BilgeSource  bilgePumpSource();
uint8_t      bilgePumpCycle();       // 1-based count of ON pulses in current sequence
uint32_t     bilgePumpPhaseMs();     // millis since current phase began

void bilgeSetManual(bool on);
