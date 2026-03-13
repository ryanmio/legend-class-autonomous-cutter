// audio.h
// DFPlayer Mini MP3 player control via software serial.
// Track numbers match AUDIO_* constants in config.h.
// Engine rumble volume tracks throttle level (call audioSetThrottle each loop).

#pragma once
#include <Arduino.h>

void audioBegin();
void audioUpdate();                    // Call every loop() for non-blocking polling
void audioPlay(uint8_t track);        // Play a specific track (stops current)
void audioStop();
void audioSetVolume(uint8_t vol);     // 0–30
void audioSetThrottle(float throttle); // 0.0–1.0; adjusts engine rumble volume
