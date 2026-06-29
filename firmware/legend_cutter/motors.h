// motors.h
// PCA9685 ESC + rudder control. Differential thrust mixing for AUTO.

#pragma once

#include <Arduino.h>

bool motorsBegin();  // returns false if PCA9685 not at PCA9685_ADDR

// Direct setters. Each clamps to safe bounds.
void setRudder(uint16_t us);
void setEscs(uint16_t us);                        // both motors same
void setEscsPortStbd(uint16_t portUs, uint16_t stbdUs);

// Mixing helpers (no I/O).
void     computePortStbd(uint16_t throttleUs, uint16_t rudderUs,
                         uint16_t& portUs, uint16_t& stbdUs);
// AUTO-only decoupled differential thrust from the raw PD yaw command (µs).
void     computeDiffThrust(uint16_t throttleUs, float yawCmd,
                           uint16_t& portUs, uint16_t& stbdUs);
uint16_t computeThrottleUs(uint16_t leftStickUs, uint16_t rightStickVUs);
uint16_t mapRudderStickToServo(uint16_t stickUs);

// Last-commanded values (for telemetry).
uint16_t motorsRudderUs();
uint16_t motorsPortUs();
uint16_t motorsStbdUs();

// Raw PCA write helper exposed for other modules (weapons, etc.) that
// share the same PCA9685 bus.
void pcaWriteUs(uint8_t pcaChannel, uint16_t us);
