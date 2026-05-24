// weapons.cpp
// Deck gun pan from test_18 (positional 9g micro on PCA ch8). Knob CH5
// maps directly to servo angle, with a ±15 µs center deadband to filter
// TX jitter and an optional reverse for this build's linkage direction.

#include "weapons.h"
#include "config.h"
#include "ibus.h"
#include "motors.h"   // pcaWriteUs()

static uint16_t panUs = NEUTRAL_US;

static uint16_t clampPan(uint16_t rawUs) {
    if (rawUs < 1000) rawUs = 1000;
    if (rawUs > 2000) rawUs = 2000;
    if (GUN_PAN_REVERSE) rawUs = 3000 - rawUs;
    int d = (int)rawUs - 1500;
    if (d < 0) d = -d;
    if (d <= (int)GUN_PAN_DEADBAND_US) return 1500;
    if (rawUs < GUN_PAN_MIN_US) rawUs = GUN_PAN_MIN_US;
    if (rawUs > GUN_PAN_MAX_US) rawUs = GUN_PAN_MAX_US;
    return rawUs;
}

void weaponsBegin() {
    panUs = NEUTRAL_US;
    pcaWriteUs(CH_GUN_PAN, panUs);
}

void weaponsUpdate() {
    // No RC = freeze at neutral. The mode-FSM in the .ino handles full
    // failsafe; here we just track the knob whenever RC is alive.
    if (!ibusEverGood()) {
        if (panUs != NEUTRAL_US) {
            panUs = NEUTRAL_US;
            pcaWriteUs(CH_GUN_PAN, panUs);
        }
        return;
    }
    uint16_t next = clampPan(ibusChannel(IBUS_IDX_GUN_PAN));
    if (next != panUs) {
        panUs = next;
        pcaWriteUs(CH_GUN_PAN, panUs);
    }
}

uint16_t weaponsGunPanUs() { return panUs; }
