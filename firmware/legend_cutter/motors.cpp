// motors.cpp
// PCA9685-based motor and servo control.
//
// Differential thrust mixing:
//   When the rudder is deflected, the inside motor is slowed and the outside
//   motor is sped up by DIFF_THRUST_FACTOR × rudder_deflection.
//   This supplements the mechanical rudder for tighter low-speed turns.
//
// Twin screws are counter-rotating — port CW, starboard CCW.
// Both ESCs use standard servo PWM: 1000 µs = full reverse, 1500 = stop, 2000 = full fwd.
// ESCs must be armed with throttle at 1500 µs on power-up.

#include "motors.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

static Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(PCA9685_ADDR);

// Convert microseconds to PCA9685 tick count (12-bit, 0–4095 at PCA9685_FREQ Hz)
static uint16_t usToPCA(uint16_t us) {
  // Period in µs = 1,000,000 / freq
  float periodUs = 1000000.0f / PCA9685_FREQ;
  return (uint16_t)((us / periodUs) * 4096);
}

void motorsBegin() {
  pca.begin();
  pca.setOscillatorFrequency(27000000);  // Trim if needed
  pca.setPWMFreq(PCA9685_FREQ);
  delay(10);
  motorsKill();  // Safe state on boot
}

void pcaSetMicros(uint8_t channel, uint16_t us) {
  us = constrain(us, PWM_MIN, PWM_MAX);
  pca.setPWM(channel, 0, usToPCA(us));
}

void motorsKill() {
  pcaSetMicros(CH_ESC_PORT, PWM_NEUTRAL);
  pcaSetMicros(CH_ESC_STBD, PWM_NEUTRAL);
  pcaSetMicros(CH_RUDDER,   PWM_NEUTRAL);
}

void motorsSetManual(uint16_t throttleUs, uint16_t rudderUs) {
  // Rudder deflection as fraction (-1.0 to +1.0)
  float rudderDelta = (float)(rudderUs - PWM_NEUTRAL) / (float)(PWM_MAX - PWM_NEUTRAL);

  // Differential thrust adjustment (µs)
  float diffUs = rudderDelta * DIFF_THRUST_FACTOR * (throttleUs - PWM_NEUTRAL);

  uint16_t portUs  = constrain((int)throttleUs + (int)diffUs,  PWM_MIN, PWM_MAX);
  uint16_t stbdUs  = constrain((int)throttleUs - (int)diffUs,  PWM_MIN, PWM_MAX);

  pcaSetMicros(CH_ESC_PORT, portUs);
  pcaSetMicros(CH_ESC_STBD, stbdUs);
  pcaSetMicros(CH_RUDDER,   rudderUs);
}

void motorsSetAuto(float headingError, float throttleUs) {
  // TODO (Phase 3): Heading-hold PID drives rudder and differential thrust.
  // headingError: degrees, positive = need to turn right.
  // Placeholder: pass through without modification.
  motorsSetManual((uint16_t)throttleUs, PWM_NEUTRAL);
}

// ---- Weapons / accessories ----

void setGunPan(uint16_t us)  { pcaSetMicros(CH_GUN_PAN,  us); }
void setGunTilt(uint16_t us) { pcaSetMicros(CH_GUN_TILT, us); }
void setCIWSPan(uint16_t us) { pcaSetMicros(CH_CIWS_PAN, us); }

void setCIWSSpin(bool run) {
  // L9110S motor driver: full PWM on = spin, neutral = stop
  pcaSetMicros(CH_CIWS_SPIN, run ? PWM_MAX : PWM_NEUTRAL);
}

void setRadar(bool run) {
  // Simple on/off via MOSFET gate on RADAR_MOTOR_PIN (not PCA9685)
  digitalWrite(RADAR_MOTOR_PIN, run ? HIGH : LOW);
}

void setBayDoor(uint8_t side, int8_t dir) {
  uint8_t ch = (side == 0) ? CH_BAY_DOOR_PORT : CH_BAY_DOOR_STBD;
  uint16_t us = (dir > 0) ? 1600 : (dir < 0) ? 1400 : PWM_NEUTRAL;
  pcaSetMicros(ch, us);
}

void setAnchor(uint8_t which, int8_t dir) {
  uint8_t ch = (which == 0) ? CH_ANCHOR_FWD : CH_ANCHOR_AFT;
  uint16_t us = (dir > 0) ? 1600 : (dir < 0) ? 1400 : PWM_NEUTRAL;
  pcaSetMicros(ch, us);
}
