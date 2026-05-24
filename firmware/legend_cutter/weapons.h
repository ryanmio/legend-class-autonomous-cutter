// weapons.h
// Deck gun pan only (test_18 PASS 2026-05-04). Knob CH5 → PCA ch8
// positional servo. CIWS / anchor / bay doors are NOT integrated — add
// them back when each subsystem has its own water-tested gate.

#pragma once

#include <Arduino.h>

void weaponsBegin();
void weaponsUpdate();   // tracks the CH5 knob into the pan servo

// Last-commanded pan µs (for telemetry).
uint16_t weaponsGunPanUs();
