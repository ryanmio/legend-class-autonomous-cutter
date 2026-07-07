// weapons.cpp
// Deck gun pan from test_18 (positional 9g micro on PCA ch8). Knob CH5
// maps directly to servo angle, with a ±15 µs center deadband to filter
// TX jitter and an optional reverse for this build's linkage direction.

#include "weapons.h"
#include "config.h"
#include "ibus.h"
#include "motors.h"   // pcaWriteUs()

static uint16_t panUs  = NEUTRAL_US;
static bool     primed = false;   // false until the servo has been driven once

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
    // Park at neutral but don't drive the servo yet — its boot snap draws a
    // current spike on the shared rail while the DFPlayer is powering up. The
    // first drive happens on the first weaponsUpdate() pass instead.
    panUs  = NEUTRAL_US;
    primed = false;
}

void weaponsUpdate() {
    // No RC = ease back to neutral. The mode-FSM in the .ino handles full
    // failsafe; here we just track the knob whenever RC is alive.
    uint16_t target = ibusEverGood() ? clampPan(ibusChannel(IBUS_IDX_GUN_PAN))
                                     : NEUTRAL_US;

    // Establish the neutral hold once, then only ever slew from there.
    if (!primed) {
        primed = true;
        pcaWriteUs(CH_GUN_PAN, panUs);
    }
    if (panUs == target) return;

    // Move toward the target by at most one step per pass so the servo ramps
    // into position instead of slamming.
    if (panUs < target) {
        uint16_t d = target - panUs;
        panUs += (d < GUN_PAN_SLEW_STEP_US) ? d : GUN_PAN_SLEW_STEP_US;
    } else {
        uint16_t d = panUs - target;
        panUs -= (d < GUN_PAN_SLEW_STEP_US) ? d : GUN_PAN_SLEW_STEP_US;
    }
    pcaWriteUs(CH_GUN_PAN, panUs);
}

uint16_t weaponsGunPanUs() { return panUs; }
