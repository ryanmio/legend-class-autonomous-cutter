// lights.cpp

#include "lights.h"
#include "config.h"

static bool navOn = false, bridgeOn = false, deckOn = false;

static uint8_t pinOf(LedId id) {
    switch (id) {
      case LED_NAV:    return PIN_NAV;
      case LED_BRIDGE: return PIN_BRIDGE;
      case LED_DECK:   return PIN_DECK;
    }
    return 0;
}

static bool* stateOf(LedId id) {
    switch (id) {
      case LED_NAV:    return &navOn;
      case LED_BRIDGE: return &bridgeOn;
      case LED_DECK:   return &deckOn;
    }
    return nullptr;
}

void lightsBegin() {
    pinMode(PIN_NAV,    OUTPUT); digitalWrite(PIN_NAV,    LOW);
    pinMode(PIN_BRIDGE, OUTPUT); digitalWrite(PIN_BRIDGE, LOW);
    pinMode(PIN_DECK,   OUTPUT); digitalWrite(PIN_DECK,   LOW);
}

bool lightsSet(LedId id, bool on) {
    bool* s = stateOf(id);
    if (!s) return false;
    *s = on;
    digitalWrite(pinOf(id), on ? HIGH : LOW);
    return true;
}

bool lightsState(LedId id) {
    bool* s = stateOf(id);
    return s ? *s : false;
}
