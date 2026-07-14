// lowvolt.cpp — see lowvolt.h. Annunciation only; touches no control output.

#include "lowvolt.h"
#include "config.h"
#include "battery.h"
#include "lights.h"

// Nav-light annunciation: a double-blink, distinct from steady on/off (the only
// nav states operator /led control produces). Two short flashes at the start of
// each 1 s cycle, then dark. Non-blocking — the pattern is derived from millis().
static const uint32_t FLASH_CYCLE_MS = 1000;
static const uint32_t FLASH_BLINK1_END_MS = 120;   // on  [0,120)
static const uint32_t FLASH_BLINK2_BEG_MS = 240;   // off [120,240)
static const uint32_t FLASH_BLINK2_END_MS = 360;   // on  [240,360), then dark to cycle end

static volatile bool latched = false;   // written on core 1 only; read cross-core (volatile)
static uint32_t inWindowSinceMs   = 0;  // trigger sustain timer
static uint32_t clearAboveSinceMs = 0;  // clear sustain timer
static bool     wasLatched = false;     // edge detect for nav-light restore

void lowVoltBegin() {
    latched = false;
    inWindowSinceMs = 0;
    clearAboveSinceMs = 0;
    wasLatched = false;
}

static void driveNavFlash() {
    uint32_t phase = millis() % FLASH_CYCLE_MS;
    bool on = (phase < FLASH_BLINK1_END_MS) ||
              (phase >= FLASH_BLINK2_BEG_MS && phase < FLASH_BLINK2_END_MS);
    digitalWrite(PIN_NAV, on ? HIGH : LOW);
}

void lowVoltUpdate() {
    // No INA219 → batt_v can't be trusted as the HV bus; stay silent. (The nav
    // flash / telemetry below still honor a latch already forced for a bench test.)
    if (batteryAvailable()) {
        float v = batteryBusVolts();
        if (!latched) {
            // Trigger: sustained inside (LOWER, UPPER). Below LOWER is bench/USB
            // (silent); above UPPER is healthy. Any excursion resets the timer,
            // so transient sag under motor load never trips it.
            if (v > LOWVOLT_LOWER_V && v < LOWVOLT_UPPER_V) {
                if (inWindowSinceMs == 0) inWindowSinceMs = millis();
                if (millis() - inWindowSinceMs >= LOWVOLT_TRIGGER_SUSTAIN_MS) {
                    latched = true;
                    clearAboveSinceMs = 0;
                }
            } else {
                inWindowSinceMs = 0;
            }
        } else {
            // Clear ONLY on sustained recovery above UPPER. A dip below LOWER here
            // does not un-latch — once fired it is not a return to bench/USB.
            if (v > LOWVOLT_UPPER_V) {
                if (clearAboveSinceMs == 0) clearAboveSinceMs = millis();
                if (millis() - clearAboveSinceMs >= LOWVOLT_CLEAR_SUSTAIN_MS) {
                    latched = false;
                    inWindowSinceMs = 0;
                }
            } else {
                clearAboveSinceMs = 0;
            }
        }
    }

    if (latched) {
        driveNavFlash();               // overrides the operator's nav-light state while latched
    } else if (wasLatched) {
        lightsSet(LED_NAV, lightsState(LED_NAV));   // restore the logical nav state on clear
    }
    wasLatched = latched;
}

bool lowVoltActive() { return latched; }

void lowVoltForceLatch() {
    latched = true;
    clearAboveSinceMs = 0;
}
