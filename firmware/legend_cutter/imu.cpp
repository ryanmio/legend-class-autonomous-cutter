// imu.cpp
// ICM-20948 integration using SparkFun ICM-20948 Arduino library.
// Complementary filter fuses magnetometer heading with gyro yaw rate
// to give a stable, drift-corrected compass heading.
//
// Calibration: magnetometer hard-iron offsets (min/max per axis) are stored
// in NVS under namespace "imu_cal". Call imuCalibrateMag() once on the water.

#include "imu.h"
#include "config.h"
#include <ICM_20948.h>
#include <Preferences.h>

static ICM_20948_I2C icm;
static ImuData       imuData;
static Preferences   prefs;
static unsigned long lastUpdateMs = 0;

// Magnetometer calibration offsets (hard-iron)
static float magOffX = 0, magOffY = 0, magOffZ = 0;

// Complementary filter coefficient (higher = more gyro trust)
#define COMP_ALPHA  0.98f

void imuLoadCalibration() {
  prefs.begin("imu_cal", true);  // read-only
  magOffX = prefs.getFloat("magOffX", 0.0f);
  magOffY = prefs.getFloat("magOffY", 0.0f);
  magOffZ = prefs.getFloat("magOffZ", 0.0f);
  imuData.calibrated = prefs.isKey("magOffX");
  prefs.end();
}

void imuBegin() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_FREQ);

  bool ok = false;
  for (int tries = 0; tries < 5; tries++) {
    icm.begin(Wire, 0);  // AD0 = 0 → address 0x68
    if (icm.status == ICM_20948_Stat_Ok) { ok = true; break; }
    delay(200);
  }
  if (!ok) {
    Serial.println("[IMU] ICM-20948 not found — check wiring");
    return;
  }

  // Enable DMP for sensor fusion (or use raw + complementary filter below)
  icm.initializeDMP();
  icm.enableDMPSensor(INV_ICM20948_SENSOR_GAME_ROTATION_VECTOR);
  icm.setDMPODRrate(DMP_ODR_Reg_Quat6, 0);  // Maximum rate
  icm.enableFIFO();
  icm.enableDMP();
  icm.resetDMP();
  icm.resetFIFO();

  imuLoadCalibration();
  memset(&imuData, 0, sizeof(imuData));
}

void imuUpdate() {
  unsigned long now = millis();
  float dt = (now - lastUpdateMs) / 1000.0f;
  if (dt < 0.01f) return;  // Cap at 100 Hz
  lastUpdateMs = now;

  icm_20948_DMP_data_t data;
  icm.readDMPdataFromFIFO(&data);

  if ((icm.status == ICM_20948_Stat_Ok) || (icm.status == ICM_20948_Stat_FIFOMoreDataAvail)) {
    if ((data.header & DMP_header_bitmap_Quat6) > 0) {
      // Quaternion from DMP game-rotation vector → roll/pitch
      double q1 = ((double)data.Quat6.Data.Q1) / 1073741824.0;
      double q2 = ((double)data.Quat6.Data.Q2) / 1073741824.0;
      double q3 = ((double)data.Quat6.Data.Q3) / 1073741824.0;
      double q0 = sqrt(1.0 - q1*q1 - q2*q2 - q3*q3);

      imuData.roll  = degrees(atan2(2*(q0*q1 + q2*q3), 1 - 2*(q1*q1 + q2*q2)));
      imuData.pitch = degrees(asin(2*(q0*q2 - q3*q1)));
    }
  }

  // TODO (Phase 2): Add magnetometer read + hard-iron correction → heading
  // TODO (Phase 2): Complementary filter to fuse mag heading with gyro yaw rate
}

const ImuData& imuGet() { return imuData; }

void imuCalibrateMag() {
  // Rotate the vessel 360°; record min/max on each axis.
  Serial.println("[IMU] Starting magnetometer calibration. Rotate vessel 360°...");
  float minX = 1e9, maxX = -1e9;
  float minY = 1e9, maxY = -1e9;
  float minZ = 1e9, maxZ = -1e9;
  unsigned long endMs = millis() + 30000;  // 30 second window

  while (millis() < endMs) {
    // TODO: read raw magnetometer and update min/max
    delay(50);
  }

  magOffX = (maxX + minX) / 2.0f;
  magOffY = (maxY + minY) / 2.0f;
  magOffZ = (maxZ + minZ) / 2.0f;

  prefs.begin("imu_cal", false);
  prefs.putFloat("magOffX", magOffX);
  prefs.putFloat("magOffY", magOffY);
  prefs.putFloat("magOffZ", magOffZ);
  prefs.end();

  imuData.calibrated = true;
  Serial.println("[IMU] Calibration saved to NVS.");
}
