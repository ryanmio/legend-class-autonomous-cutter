// sonar.cpp
// JSN-SR04T depth sonar driver.
// Ping cycle: pull TRIG HIGH for 10 µs → wait for ECHO HIGH → measure HIGH duration.
// Depth = (echo_duration_µs / 2) / SONAR_SOUND_SPEED_US_CM / 100 metres.
//
// IMPORTANT: SONAR_SOUND_SPEED_US_CM must be set for water (~13.4), NOT air (58).
// The JSN-SR04T V3 has a minimum range of ~20 cm and maximum of ~10 m in water.
// Pings issued at 100 ms intervals; blocking pulseIn() used here — replace with
// interrupt-driven capture if main-loop latency becomes an issue.

#include "sonar.h"
#include "config.h"

static SonarData    sonarData;
static unsigned long lastPingMs = 0;
static const unsigned long PING_INTERVAL_MS = 100;
static const unsigned long ECHO_TIMEOUT_US  = 30000;  // ~10 m max range in water

void sonarBegin() {
  pinMode(SONAR_TRIG_PIN, OUTPUT);
  pinMode(SONAR_ECHO_PIN, INPUT);
  digitalWrite(SONAR_TRIG_PIN, LOW);
  memset(&sonarData, 0, sizeof(sonarData));
}

void sonarUpdate() {
  unsigned long now = millis();
  if (now - lastPingMs < PING_INTERVAL_MS) return;
  lastPingMs = now;

  // Issue 10 µs trigger pulse
  digitalWrite(SONAR_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(SONAR_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(SONAR_TRIG_PIN, LOW);

  long duration = pulseIn(SONAR_ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  sonarData.lastPingMs = millis();

  if (duration == 0) {
    sonarData.valid  = false;
    sonarData.depthM = 0.0f;
  } else {
    // Distance to bottom = (duration / 2) / speed_of_sound_us_per_cm / 100
    sonarData.depthM = ((float)duration / 2.0f) / SONAR_SOUND_SPEED_US_CM / 100.0f;
    sonarData.valid  = true;
  }
}

const SonarData& sonarGet() { return sonarData; }
