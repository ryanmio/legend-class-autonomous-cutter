// imu.h
// ICM-20948 9-DOF IMU (I2C, SparkFun library).
// Provides compass heading (magnetometer + gyro fusion), roll, and pitch.
// Mounted in mast ~30–40 cm above steel ballast to minimise magnetic interference.
// Magnetometer requires one-time spin calibration; offsets stored in NVS flash.

#pragma once
#include <Arduino.h>

struct ImuData {
  float heading;    // Fused compass heading, degrees 0–360 (0 = true/magnetic north)
  float roll;       // Degrees, positive = starboard down
  float pitch;      // Degrees, positive = bow up
  float yawRate;    // Degrees/sec from gyro (for heading-hold PID derivative)
  bool  calibrated; // true = magnetometer calibration offsets loaded
};

void imuBegin();
void imuUpdate();              // Call every loop(); applies complementary filter
const ImuData& imuGet();

// Trigger interactive spin calibration (rotates 360°, records min/max).
// Saves offsets to NVS. Call from app Settings → "Calibrate Compass".
void imuCalibrateMag();

// Load magnetometer offsets from NVS flash
void imuLoadCalibration();
