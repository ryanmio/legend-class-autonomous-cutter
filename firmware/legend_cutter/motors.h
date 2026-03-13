// motors.h
// Motor and servo control via PCA9685 16-channel PWM driver.
// Manages port/starboard ESCs, rudder, and differential thrust mixing.
// All channels from config.h CH_* constants.

#pragma once
#include <Arduino.h>

void motorsBegin();

// Manual passthrough: map iBUS channel values (1000–2000) directly to actuators.
// Applies differential thrust mixing: rudder deflection adjusts port vs stbd speeds.
void motorsSetManual(uint16_t throttleUs, uint16_t rudderUs);

// Autonomous: set desired heading and throttle; internal mixing applies.
void motorsSetAuto(float headingError, float throttleUs);

// Immediately stop both ESCs and centre rudder (failsafe / e-stop)
void motorsKill();

// Set a single PCA9685 channel to a pulse width in microseconds
void pcaSetMicros(uint8_t channel, uint16_t us);

// Weapons / accessories — convenience wrappers
void setGunPan(uint16_t us);
void setGunTilt(uint16_t us);
void setCIWSPan(uint16_t us);
void setCIWSSpin(bool run);        // Barrel spin motor on/off
void setRadar(bool run);           // Radar rotation on/off
void setBayDoor(uint8_t side, int8_t dir);   // side: 0=port 1=stbd, dir: 1=open -1=close 0=stop
void setAnchor(uint8_t which, int8_t dir);   // which: 0=fwd 1=aft, dir: 1=lower -1=raise 0=stop
