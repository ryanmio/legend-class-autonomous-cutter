// lights.h
// Three independent LED circuits: nav (running lights), bridge, deck.
// Controlled from the app via /led.

#pragma once

#include <Arduino.h>

enum LedId {
    LED_NAV    = 0,
    LED_BRIDGE = 1,
    LED_DECK   = 2,
};

void lightsBegin();
bool lightsSet(LedId id, bool on);
bool lightsState(LedId id);
