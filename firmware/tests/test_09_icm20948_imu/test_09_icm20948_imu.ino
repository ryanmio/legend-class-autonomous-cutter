/*
 * test_09_icm20948_imu.ino
 *
 * Bench test. Confirms the SparkFun ICM-20948 IMU is alive on I2C and
 * delivers reasonable accel/gyro/mag data. Prints (uncalibrated)
 * heading / pitch / roll at 2 Hz.
 *
 * On startup, scans the I2C bus and reports which expected devices were
 * found:
 *   0x40  PCA9685   (servo driver)
 *   0x41  INA219    (current sensor)
 *   0x68  ICM-20948 (this test's target)
 *   0x70  PCA9685 all-call broadcast (normal, not a separate device)
 *
 * Wiring:
 *   ICM-20948 SDA  → GPIO 21
 *   ICM-20948 SCL  → GPIO 22
 *   ICM-20948 VCC  → ESP32 3.3V
 *   ICM-20948 GND  → GND
 *   ICM-20948 AD0  → low (default address 0x68)
 *
 * Library: install "SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library"
 * via Library Manager.
 */

#include <Wire.h>
#include "ICM_20948.h"

ICM_20948_I2C myICM;

const uint8_t IMU_ADDR = 0x68;
const uint8_t PCA_ADDR = 0x40;
const uint8_t INA_ADDR = 0x41;

unsigned long lastPrint    = 0;
unsigned long lastWarn     = 0;
const unsigned long PRINT_INTERVAL_MS = 500;   // 2 Hz

void scanI2C(bool &foundIMU) {
  Serial.println("--- I2C bus scan (SDA=GPIO21, SCL=GPIO22) ---");
  bool foundPCA = false, foundINA = false, foundAllCall = false;
  foundIMU = false;
  int total = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      total++;
      Serial.printf("  0x%02X", addr);
      if      (addr == IMU_ADDR) { Serial.print("  <- ICM-20948 (IMU)");          foundIMU = true; }
      else if (addr == PCA_ADDR) { Serial.print("  <- PCA9685 (servo driver)");   foundPCA = true; }
      else if (addr == INA_ADDR) { Serial.print("  <- INA219 (current sensor)");  foundINA = true; }
      else if (addr == 0x70)     { Serial.print("  (PCA9685 all-call, normal)");  foundAllCall = true; }
      Serial.println();
    }
  }
  Serial.printf("%d device(s) found.\n", total);
  Serial.println();
  Serial.printf("  ICM-20948 (0x%02X): %s\n", IMU_ADDR, foundIMU ? "FOUND"  : "MISSING");
  Serial.printf("  PCA9685   (0x%02X): %s\n", PCA_ADDR, foundPCA ? "FOUND"  : "missing (OK if unwired)");
  Serial.printf("  INA219    (0x%02X): %s\n", INA_ADDR, foundINA ? "FOUND"  : "missing (OK if unwired)");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_09_icm20948_imu");
  Serial.println("========================================");

  Wire.begin(21, 22);
  Wire.setClock(400000);

  bool foundIMU = false;
  scanI2C(foundIMU);

  if (!foundIMU) {
    Serial.println("[FAIL] ICM-20948 not detected on I2C bus.");
    Serial.println("  Check: SDA=21, SCL=22, VCC=3.3V, GND, AD0 low for 0x68.");
    Serial.println("  If AD0 is high the address would be 0x69 — adjust IMU_ADDR.");
    while (true) delay(1000);
  }

  Serial.println("Initializing ICM-20948...");
  bool initOK = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    myICM.begin(Wire, 0);  // 0 = AD0 low → 0x68
    if (myICM.status == ICM_20948_Stat_Ok) { initOK = true; break; }
    Serial.printf("  attempt %d: %s\n", attempt, myICM.statusString());
    delay(500);
  }

  if (!initOK) {
    Serial.println();
    Serial.println("[FAIL] ICM-20948 init failed (sensor seen on bus, but begin() returned error).");
    Serial.println("  Possible causes: bad solder joint on the ICM, intermittent SDA/SCL, ");
    Serial.println("  or magnetometer init timeout — try power-cycling the board.");
    while (true) delay(1000);
  }

  Serial.println();
  Serial.println("[PASS] ICM-20948 initialized.");
  Serial.println("Streaming heading / pitch / roll at 2 Hz. Tilt and rotate the board.");
  Serial.println("----------------------------------------");
  lastPrint = millis();
}

void loop() {
  if (millis() - lastPrint < PRINT_INTERVAL_MS) return;
  if (!myICM.dataReady()) return;

  myICM.getAGMT();  // accel + gyro + mag + temp into internal struct

  // Accel in mg, mag in µT (per SparkFun lib defaults).
  float ax = myICM.accX();
  float ay = myICM.accY();
  float az = myICM.accZ();
  float mx = myICM.magX();
  float my = myICM.magY();
  float mz = myICM.magZ();

  float accMag = sqrtf(ax*ax + ay*ay + az*az);

  // Sanity: a stationary sensor should read ~1000 mg total. Way off → warn.
  if ((accMag < 100.0f || accMag > 5000.0f) && millis() - lastWarn > 2000) {
    Serial.printf("[WARN] accel magnitude %.0f mg is far from 1g — sensor stuck or noisy?\n", accMag);
    lastWarn = millis();
  }

  // Roll & pitch from accel (textbook formulas).
  float roll  = atan2f(ay, az) * 180.0f / PI;
  float pitch = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / PI;

  // Tilt-compensated heading (Honeywell AN-203). NOTE: the AK09916
  // magnetometer inside the ICM-20948 has axes that don't perfectly align
  // with the accel/gyro frame, and we are not applying hard/soft-iron
  // calibration. Treat the heading as a relative number that should change
  // smoothly when the board rotates — NOT as a calibrated true bearing.
  float rollR  = roll  * PI / 180.0f;
  float pitchR = pitch * PI / 180.0f;
  float Bx = mx * cosf(pitchR)
           + my * sinf(rollR) * sinf(pitchR)
           + mz * cosf(rollR) * sinf(pitchR);
  float By = my * cosf(rollR) - mz * sinf(rollR);
  float heading = atan2f(-By, Bx) * 180.0f / PI;
  if (heading < 0) heading += 360.0f;

  Serial.printf("Heading: %6.1f°   Pitch: %+6.1f°   Roll: %+6.1f°   |a|=%4.0f mg\n",
                heading, pitch, roll, accMag);

  lastPrint = millis();
}
