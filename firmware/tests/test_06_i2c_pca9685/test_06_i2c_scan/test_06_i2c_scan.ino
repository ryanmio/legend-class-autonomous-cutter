/*
 * test_06_i2c_scan.ino
 * Scans the I2C bus (SDA=21, SCL=22) and reports all found devices.
 * Expected addresses:
 *   0x40 — PCA9685 (servo driver)
 *   0x41 — INA219 (current/voltage monitor, if wired)
 *   0x68 or 0x69 — ICM-20948 IMU (if wired)
 * Pass: 0x40 appears in the scan output.
 */

#include <Wire.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(21, 22);  // SDA=21, SCL=22
  Serial.println("\n--- I2C Bus Scan ---");
  Serial.println("SDA=GPIO21  SCL=GPIO22\n");

  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.print("  Found device at 0x");
      if (addr < 16) Serial.print("0");
      Serial.print(addr, HEX);

      if (addr == 0x40) Serial.print("  <-- PCA9685 (servo driver)");
      if (addr == 0x41) Serial.print("  <-- INA219 (power monitor)");
      if (addr == 0x68) Serial.print("  <-- ICM-20948 IMU (AD0 low)");
      if (addr == 0x69) Serial.print("  <-- ICM-20948 IMU (AD0 high)");
      Serial.println();
      found++;
    }
  }

  if (found == 0) {
    Serial.println("  No I2C devices found.");
    Serial.println("  Check: SDA/SCL wires swapped? PCA9685 VCC on 3.3V?");
  } else {
    Serial.print("\n");
    Serial.print(found);
    Serial.println(" device(s) found.");

    if (found > 0) {
      // Quick check for required device
      Wire.beginTransmission(0x40);
      if (Wire.endTransmission() == 0) {
        Serial.println("PASS: PCA9685 at 0x40 confirmed.");
      } else {
        Serial.println("FAIL: PCA9685 at 0x40 not found.");
      }
    }
  }

  Serial.println("\nScan complete. Open Serial Monitor at 115200 baud.");
}

void loop() {
  // nothing — scan runs once in setup()
}
