// floodalarm.cpp — see floodalarm.h. Annunciation only; touches no control output.

#include "floodalarm.h"
#include "config.h"
#include "bilge.h"
#include "lights.h"
#include "lowvolt.h"

// Nav-LED annunciation: a fast continuous strobe (~5 Hz, on 100 / off 100),
// unmistakable against low-volt's counted double-blink and the pump's counted
// triple-blink. Non-blocking — derived from millis().
static const uint32_t STROBE_PERIOD_MS = 200;
static const uint32_t STROBE_ON_MS     = 100;

static volatile bool latched = false;   // written on core 1 only; read cross-core (volatile)
static uint32_t wetSinceMs   = 0;       // trigger sustain timer
static uint32_t drySinceMs   = 0;       // clear sustain timer
static bool     wasStrobing  = false;   // edge detect for nav-light restore

void floodAlarmBegin() {
    latched     = false;
    wetSinceMs  = 0;
    drySinceMs  = 0;
    wasStrobing = false;
}

static void driveNavStrobe() {
    bool on = (millis() % STROBE_PERIOD_MS) < STROBE_ON_MS;
    digitalWrite(PIN_NAV, on ? HIGH : LOW);
}

void floodAlarmUpdate() {
    bool wet = bilgeFwdWet() || bilgeMidWet();

    if (!latched) {
        // Trigger: fwd OR mid wet, sustained. Any dry sample resets the timer, so
        // probe chatter in chop can't trip it below FLOOD_TRIGGER_SUSTAIN_MS.
        if (wet) {
            if (wetSinceMs == 0) wetSinceMs = millis();
            if (millis() - wetSinceMs >= FLOOD_TRIGGER_SUSTAIN_MS) {
                latched    = true;
                drySinceMs = 0;
            }
        } else {
            wetSinceMs = 0;
        }
    } else {
        // Clear: BOTH probes dry, sustained long. Any wet sample resets the timer,
        // so a wave sloshing a probe dry can't silence a still-present flood.
        if (!wet) {
            if (drySinceMs == 0) drySinceMs = millis();
            if (millis() - drySinceMs >= FLOOD_CLEAR_SUSTAIN_MS) {
                latched    = false;
                wetSinceMs = 0;
            }
        } else {
            drySinceMs = 0;
        }
    }

    // Shared nav LED, priority low-volt > flood > pump-run. Strobe only when
    // low-volt is not annunciating (low-volt outranks flood). The restore is
    // guarded on !lowVoltActive() so flood never stomps low-volt's double-blink on
    // the iteration low-volt rises; on flood-off it restores the operator's nav.
    bool strobing = latched && !lowVoltActive();
    if (strobing) {
        driveNavStrobe();
    } else if (wasStrobing && !lowVoltActive()) {
        lightsSet(LED_NAV, lightsState(LED_NAV));
    }
    wasStrobing = strobing;
}

bool floodAlarmActive() { return latched; }
