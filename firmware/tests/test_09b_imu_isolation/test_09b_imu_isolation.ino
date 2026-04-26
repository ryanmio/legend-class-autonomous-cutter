/*
 * test_09b_imu_isolation.ino
 *
 * DIAGNOSTIC sketch — for proving whether the SparkFun ICM-20948 is alive
 * on its own. Run this with the IMU as the ONLY device on the I2C bus
 * (no PCA9685, no INA219, just the IMU).
 *
 * Each scan iteration:
 *   1. Sweeps every I2C address (1..126) and reports anything that ACKs.
 *   2. If 0x68 OR 0x69 ACKs, attempts to read the WHO_AM_I register.
 *      ICM-20948 should return 0xEA. Anything else means the chip isn't
 *      really replying like an ICM-20948 (damaged, or wrong silicon).
 *   3. Reports a clear PASS / FAIL line with WHY.
 *   4. Repeats every 3 seconds so you can wiggle wires while watching.
 *
 * Wiring:
 *   IMU Qwiic cable → ESP32 SDA=GPIO21, SCL=GPIO22, 3.3V, GND
 *   AD0 ground pigtail OK to leave attached (forces 0x68).
 *
 * Bus speed is intentionally throttled to 100 kHz for diagnostics.
 */

#include <Wire.h>

const uint8_t ICM_WHOAMI_REG = 0x00;
const uint8_t ICM_WHOAMI_VAL = 0xEA;

unsigned long iteration = 0;

// Try to read one byte from `reg` on `addr`. Returns true on success and
// puts the byte in `out`. Reports the failure reason on serial otherwise.
bool readReg(uint8_t addr, uint8_t reg, uint8_t &out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  uint8_t err = Wire.endTransmission(false);  // repeated start
  if (err != 0) {
    Serial.printf("    [fail] register-write phase err=%u\n", err);
    Serial.println("           (1=data too long, 2=NACK on addr, 3=NACK on data,");
    Serial.println("            4=other, 5=timeout — bus might be locked low)");
    return false;
  }
  size_t got = Wire.requestFrom(addr, (uint8_t)1);
  if (got != 1) {
    Serial.printf("    [fail] requestFrom returned %u bytes (expected 1)\n",
                  (unsigned)got);
    return false;
  }
  out = Wire.read();
  return true;
}

void scanOnce() {
  iteration++;
  Serial.println();
  Serial.printf("--- scan #%lu  (SDA=GPIO21, SCL=GPIO22, 100 kHz) ---\n",
                iteration);

  int total = 0;
  uint8_t imuAddr = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      total++;
      Serial.printf("  ack at 0x%02X", addr);
      if (addr == 0x68 || addr == 0x69) {
        Serial.print("  <- ICM-20948 candidate");
        if (imuAddr == 0) imuAddr = addr;
      }
      Serial.println();
    }
  }
  Serial.printf("  %d device(s) acked.\n", total);

  if (total == 0) {
    Serial.println();
    Serial.println("  RESULT: nothing on the bus.");
    Serial.println("  → Suspect: bus pull-ups missing, SDA/SCL swapped at IMU,");
    Serial.println("    cable/connector seated wrong, or chip not powered.");
    Serial.println("  Check 3.3V at the IMU's VCC pad with a multimeter.");
    return;
  }

  if (imuAddr == 0) {
    Serial.println();
    Serial.println("  RESULT: something is on the bus, but not at 0x68 or 0x69.");
    Serial.println("  → IMU is not responding at either default address.");
    Serial.println("    Check AD0 wiring; verify this is actually an ICM-20948.");
    return;
  }

  Serial.printf("  reading WHO_AM_I (reg 0x%02X) on 0x%02X...\n",
                ICM_WHOAMI_REG, imuAddr);
  uint8_t whoami = 0;
  if (!readReg(imuAddr, ICM_WHOAMI_REG, whoami)) {
    Serial.println();
    Serial.println("  RESULT: device acked address but failed register read.");
    Serial.println("  → Bus is partially working but the chip isn't completing");
    Serial.println("    transactions. Possible chip damage or clock-stretch lockup.");
    return;
  }

  Serial.printf("  WHO_AM_I = 0x%02X  (expected 0x%02X)\n",
                whoami, ICM_WHOAMI_VAL);
  if (whoami == ICM_WHOAMI_VAL) {
    Serial.println();
    Serial.printf("  [PASS] ICM-20948 alive at 0x%02X. Chip is fine.\n", imuAddr);
    Serial.println("         If it dies on the shared bus, the problem is");
    Serial.println("         shared-bus interaction (pull-up stacking, capacitance,");
    Serial.println("         or another device) — NOT the IMU itself.");
  } else {
    Serial.println();
    Serial.println("  RESULT: device responded but WHO_AM_I value is wrong.");
    Serial.println("  → This silicon is not behaving as an ICM-20948.");
    Serial.println("    Possible: damaged register file, counterfeit chip, or");
    Serial.println("    you're addressing a different device that happens to ack.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("========================================");
  Serial.println("  test_09b_imu_isolation");
  Serial.println("  IMU should be the ONLY device on I2C.");
  Serial.println("========================================");
  Wire.begin(21, 22);
  Wire.setClock(100000);  // slow for diagnostics
  scanOnce();
}

void loop() {
  delay(3000);
  scanOnce();
}
