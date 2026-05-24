// radar.cpp
// LEDC PWM on PIN_RADAR_MOTOR @ 1 kHz, 8-bit duty. Burst FSM toggles
// between burn (PWM @ speed) and pause (PWM off) phases on a millis()
// timer. millis()-based timing jitters ~1-2 ms under WiFi + iBUS + IMU
// load, so burst_ms < ~5 produces variable step sizes (acceptable for
// cosmetic radar).

#include "radar.h"
#include "config.h"

static bool     on        = false;
static uint8_t  speedPct  = RADAR_DEFAULT_SPEED;
static uint32_t burstMs   = RADAR_BURST_MS_DEFAULT;
static uint32_t pauseMs   = RADAR_PAUSE_MS_DEFAULT;

static bool     burstActive       = false;
static uint32_t burstPhaseStartMs = 0;

static void writeDuty(uint8_t pct) {
    uint32_t maxDuty = (1u << RADAR_PWM_RESOLUTION) - 1u;
    ledcWrite(PIN_RADAR_MOTOR, maxDuty * (uint32_t)pct / 100u);
}

static void applyOutput() {
    if (!on) { writeDuty(0); return; }
    burstActive       = true;
    burstPhaseStartMs = millis();
    writeDuty(speedPct);
}

void radarBegin() {
    // ESP32 Arduino core 3.x: ledcAttach(pin, freq, resolution) — no
    // manual channel allocation. Core 2.x's ledcSetup/ledcAttachPin is gone.
    ledcAttach(PIN_RADAR_MOTOR, RADAR_PWM_FREQ_HZ, RADAR_PWM_RESOLUTION);
    ledcWrite(PIN_RADAR_MOTOR, 0);
}

void radarSet(bool wantOn, uint8_t newSpeed, uint32_t newBurstMs, uint32_t newPauseMs) {
    if (newSpeed > 100) newSpeed = 100;
    if (newBurstMs < RADAR_BURST_MS_MIN) newBurstMs = RADAR_BURST_MS_MIN;
    if (newBurstMs > RADAR_BURST_MS_MAX) newBurstMs = RADAR_BURST_MS_MAX;
    if (newPauseMs < RADAR_PAUSE_MS_MIN) newPauseMs = RADAR_PAUSE_MS_MIN;
    if (newPauseMs > RADAR_PAUSE_MS_MAX) newPauseMs = RADAR_PAUSE_MS_MAX;
    on       = wantOn;
    speedPct = newSpeed;
    burstMs  = newBurstMs;
    pauseMs  = newPauseMs;
    applyOutput();
}

void radarUpdate() {
    if (!on) return;
    uint32_t now      = millis();
    uint32_t phaseLen = burstActive ? burstMs : pauseMs;
    if (now - burstPhaseStartMs >= phaseLen) {
        burstActive       = !burstActive;
        burstPhaseStartMs = now;
        writeDuty(burstActive ? speedPct : 0);
    }
}

bool     radarOn()       { return on; }
uint8_t  radarSpeed()    { return speedPct; }
uint32_t radarBurstMs()  { return burstMs; }
uint32_t radarPauseMs()  { return pauseMs; }
