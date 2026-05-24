// sonar.cpp
// RCWL-1655 driver. Pull TRIG LOW briefly, then HIGH for 10 µs, then read
// echo pulse. metres = µs / (SONAR_US_PER_CM × 100). pulseIn() blocks up
// to ~40 ms per ping — at ≤1 ping per 20 s this is negligible for the
// rest of the loop.

#include "sonar.h"
#include "config.h"

static DepthMode mode      = DEPTH_OFF;
static float     lastM     = -1.0f;
static uint32_t  lastReadMs = 0;

void sonarBegin() {
    pinMode(PIN_SONAR_TRIG, OUTPUT);
    digitalWrite(PIN_SONAR_TRIG, LOW);
    // PULLDOWN helps distinguish a floating pin from real idle-low during debug.
    pinMode(PIN_SONAR_ECHO, INPUT_PULLDOWN);
}

void sonarPingNow() {
    digitalWrite(PIN_SONAR_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_SONAR_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_SONAR_TRIG, LOW);

    unsigned long dur = pulseIn(PIN_SONAR_ECHO, HIGH, DEPTH_PING_TIMEOUT_US);
    lastReadMs = millis();
    lastM = (dur == 0) ? -1.0f : (float)dur / (SONAR_US_PER_CM * 100.0f);
}

void sonarSetMode(DepthMode m) {
    mode = m;
    if (m == DEPTH_OFF) {
        lastM      = -1.0f;
        lastReadMs = 0;
    } else if (m == DEPTH_RUN) {
        sonarPingNow();   // immediate first reading
    }
}

void sonarUpdate() {
    if (mode != DEPTH_RUN) return;
    if (millis() - lastReadMs >= DEPTH_RUN_INTERVAL_MS) sonarPingNow();
}

DepthMode sonarMode()         { return mode; }
float     sonarLastDepthM()   { return lastM; }
uint32_t  sonarLastReadMs()   { return lastReadMs; }
