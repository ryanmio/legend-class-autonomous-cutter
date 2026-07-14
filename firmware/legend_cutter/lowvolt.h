// lowvolt.h
// Passive low-voltage alarm — ANNUNCIATION ONLY. Never touches the ESCs, rudder,
// navigation state, or the mode FSM; RC and AUTO paths are unaffected.
//
// Reads the INA219 bus voltage (battery.h). When the HV bus sits inside the
// (LOWVOLT_LOWER_V, LOWVOLT_UPPER_V) window sustained for LOWVOLT_TRIGGER_SUSTAIN_MS
// it latches and annunciates three ways:
//   - horn (DFPlayer), repeating every LOWVOLT_HORN_INTERVAL_MS — serviced on the
//     core-0 network task so the DF UART stays single-core and the mandatory
//     pause+50 ms never blocks the control loop.
//   - nav lights, a distinct double-blink (driven here on core 1 via PIN_NAV).
//   - the low_volt_alarm telemetry boolean.
// Clears only on bus > LOWVOLT_UPPER_V sustained for LOWVOLT_CLEAR_SUSTAIN_MS. A
// dip below LOWER once latched keeps annunciating (NOT treated as a return to
// bench/USB). The latch is RAM state — a power cycle clears it.

#pragma once

#include <Arduino.h>

void lowVoltBegin();
void lowVoltUpdate();      // core 1: runs the latch SM and drives the nav-light flash
bool lowVoltActive();      // latched state; read cross-core by telemetry + horn servicer (core 0)
void lowVoltForceLatch();  // core 1 ONLY (via cmd queue): bench test path, forces the real latch
