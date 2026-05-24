// audio.h
// DFRobot DF1201S (DFPlayer Pro) — NOT the DFPlayer Mini. AT-protocol at
// 115200 over HardwareSerial2. Index-based playback only (path-based has
// a known firmware bug — DFRobot Issue #5 — that silently falls back to
// "file 1" on any path mis-resolution).

#pragma once

#include <Arduino.h>

enum AudioClip {
    AUDIO_HORN  = 0,
    AUDIO_GUN   = 1,
    AUDIO_BOARD = 2,
};

bool audioBegin();         // false if DF1201S did not ACK within ~3 s
bool audioAvailable();
void audioPlay(AudioClip clip);
