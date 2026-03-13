// sonar.h
// JSN-SR04T V3 waterproof ultrasonic transducer — hull-mounted depth sonar.
// Uses trigger/echo on GPIO 27/14.
// Speed of sound corrected for freshwater (SONAR_SOUND_SPEED_US_CM ≈ 13.4 µs/cm).
// Non-blocking: ping is issued via timer; echo captured via interrupt.

#pragma once
#include <Arduino.h>

struct SonarData {
  float   depthM;         // Depth in metres (0 if no echo / out of range)
  bool    valid;          // true if last ping returned a valid echo
  unsigned long lastPingMs;
};

void sonarBegin();
void sonarUpdate();          // Issue ping on ~100 ms interval; call every loop()
const SonarData& sonarGet();
