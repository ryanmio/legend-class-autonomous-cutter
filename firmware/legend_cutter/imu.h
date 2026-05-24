// imu.h
// ICM-20948 9-DOF + complementary-filter heading + PD heading-hold.
//
// Axis remap (confirmed water-test): chip X=up, Y=port, Z=stern →
//   ar_x = ay, ar_y = az, ar_z = ax  (accel)
//   mr_x = -mz, mr_y = -my, mr_z = -mx  (mag)

#pragma once

#include <Arduino.h>

bool  imuBegin();           // returns false if ICM-20948 not detected
void  imuUpdate();          // call every loop()
float imuHeading();         // 0..360°, fused
bool  imuHeadingReady();    // true once first sample fused

// Heading-hold rudder (in µs), clamped to [RUDDER_MIN_US, RUDDER_MAX_US].
// Uses the live PID gains; tune via setPidGains().
uint16_t imuHeadingHoldUs(float targetHeading);

void  setPidGains(float kp, float kd);
float pidKp();
float pidKd();
