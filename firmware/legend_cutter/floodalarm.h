// floodalarm.h
// Passive flood alarm — ANNUNCIATION ONLY. Never touches the ESCs, rudder,
// navigation state, or the mode FSM; RC and AUTO paths are unaffected. Reads the
// forward + mid bilge probes (bilge.h) — otherwise telemetry-only — and drives
// the shared nav LED so water pooling forward is annunciated to an operator who
// may be out of app range.
//
// One combined alarm for both probes: latches when bilgeFwdWet() || bilgeMidWet()
// is sustained for FLOOD_TRIGGER_SUSTAIN_MS. Asymmetric debounce — quick to
// trigger (reject probe chatter), slow to clear: unlatches only after both probes
// read dry for FLOOD_CLEAR_SUSTAIN_MS, so one wave sloshing a probe dry can't
// silence a real flood during a glance-away. Auto-clears; a power cycle also clears
// the RAM latch.
//
// Nav-LED annunciation: a fast continuous strobe (5 Hz), visually distinct from
// low-volt's double-blink and the pump-run triple-blink. Driven here on core 1 via
// PIN_NAV. Priority on the shared LED: low-volt > flood > pump-run > steady nav —
// flood self-gates on !lowVoltActive() (low-volt wins), and pumpNavFlashUpdate()
// gates on !floodAlarmActive() so pump-blink and flood-strobe never collide.
// Rear probe + pump behavior (bilge.cpp) are untouched.

#pragma once

#include <Arduino.h>

void floodAlarmBegin();
void floodAlarmUpdate();    // core 1: runs the latch SM and drives the nav-light strobe
bool floodAlarmActive();    // latched state; read cross-core by telemetry (core 0) + pump-flash gate
