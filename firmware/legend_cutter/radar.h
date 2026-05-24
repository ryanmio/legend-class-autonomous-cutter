// radar.h
// Mast TRS-3D dish: 3 V planetary gear motor switched by a 2N2222 NPN on
// the low side, PWM via ESP32 LEDC. Burst mode (PWM at speed for burst_ms,
// then off for pause_ms) simulates slow radar-look rotation from a too-
// fast motor; smooth PWM at any duty looks like a propeller.
//
// All controls live; tune via the /radar HTTP handler.

#pragma once

#include <Arduino.h>

void radarBegin();
void radarUpdate();        // call every loop() to step the burst FSM

void radarSet(bool on, uint8_t speedPct, uint32_t burstMs, uint32_t pauseMs);

bool     radarOn();
uint8_t  radarSpeed();
uint32_t radarBurstMs();
uint32_t radarPauseMs();
