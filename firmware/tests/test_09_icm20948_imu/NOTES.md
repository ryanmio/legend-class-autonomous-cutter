# test_09_icm20948_imu — SparkFun ICM-20948 IMU Bringup

Bench test. Confirms the ICM-20948 is on I2C, initializes via the SparkFun library, and streams heading / pitch / roll at 2 Hz. Also re-scans the bus to confirm the rest of the I2C devices are still healthy.

---

## Objective

| # | What we're checking |
|---|---------------------|
| 1 | I2C bus scan finds the ICM-20948 at 0x68 (and reports PCA9685 0x40 / INA219 0x41 status) |
| 2 | `ICM_20948::begin()` returns OK |
| 3 | Accel magnitude reads near 1 g when stationary |
| 4 | Heading / pitch / roll respond when the board is tilted and rotated |

Gates 1–2 are auto-verified at boot. Gates 3–4 are visual checks against the streaming serial output.

---

## Hardware required

- ESP32 with I2C on GPIO 21 (SDA) / 22 (SCL) — same bus as PCA9685 and INA219
- SparkFun ICM-20948 breakout (or compatible)
- Pull-ups: most SparkFun breakouts ship with 4.7 kΩ pull-ups on SDA/SCL — fine on a shared bus
- AD0 left **low** (default) for address **0x68**. Pull AD0 high if you need 0x69 — and edit `IMU_ADDR` in the sketch to match.

---

## Library required

Install via Arduino IDE → Library Manager:

> **SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library**

This sketch uses only the basic raw-sensor read path (`begin()`, `dataReady()`, `getAGMT()`, `accX/Y/Z()`, `magX/Y/Z()`). It does **not** use the ICM-20948's onboard DMP, so no library-internal `#define ICM_20948_USE_DMP` is needed.

---

## Wiring

| From | To |
|------|----|
| ICM-20948 VCC | ESP32 3.3V |
| ICM-20948 GND | ESP32 GND |
| ICM-20948 SDA | ESP32 GPIO 21 |
| ICM-20948 SCL | ESP32 GPIO 22 |
| ICM-20948 AD0 | GND (or leave floating per breakout's default — most are pulled low) |

---

## Flash and run

1. Open `test_09_icm20948_imu/test_09_icm20948_imu.ino`.
2. **Board:** ESP32 Dev Module. **Port:** your ESP32's port.
3. Upload.
4. Open Serial Monitor at **115200**.

---

## Expected output (happy path)

```
========================================
  test_09_icm20948_imu
========================================
--- I2C bus scan (SDA=GPIO21, SCL=GPIO22) ---
  0x40  <- PCA9685 (servo driver)
  0x41  <- INA219 (current sensor)
  0x68  <- ICM-20948 (IMU)
  0x70  (PCA9685 all-call, normal)
4 device(s) found.

  ICM-20948 (0x68): FOUND
  PCA9685   (0x40): FOUND
  INA219    (0x41): FOUND

Initializing ICM-20948...

[PASS] ICM-20948 initialized.
Streaming heading / pitch / roll at 2 Hz. Tilt and rotate the board.
----------------------------------------
Heading:  142.3°   Pitch:   +0.4°   Roll:   -1.1°   |a|=1003 mg
Heading:  142.5°   Pitch:   +0.5°   Roll:   -1.1°   |a|=1001 mg
Heading:   89.7°   Pitch:  +12.4°   Roll:   +5.7°   |a|= 998 mg
...
```

Sanity checks while watching the stream:

- **|a|** (accel magnitude) should hover near **1000 mg** when the board is still. Big deviations (and not because you're shaking it) indicate a stuck or noisy sensor.
- **Pitch** changes when you tilt forward / back.
- **Roll** changes when you tilt left / right.
- **Heading** changes when you yaw the board around its vertical axis. The absolute value isn't calibrated — see caveat below.

---

## Heading caveat (read this)

The heading number is **uncalibrated**. We are not:

1. Remapping the AK09916 magnetometer axes to match the accel/gyro frame (the mag chip inside the ICM is rotated ~90° relative to the accel chip).
2. Applying hard-iron / soft-iron calibration.
3. Correcting for local magnetic declination.

Effect: rotating the board *should* sweep the heading number through ~0–360° smoothly, but the value won't correspond to true compass bearing, and at certain tilts you'll see odd jumps. **That's expected for a bringup test.** A real navigation sketch will need axis remap + hard/soft-iron cal.

---

## Failure modes the sketch surfaces

| Symptom | Meaning |
|---------|---------|
| `ICM-20948 (0x68): MISSING` in scan, then `[FAIL] not detected` | Bus issue. Check SDA/SCL/VCC/GND, AD0 state, and try pulling the breakout off the I2C trunk to test it alone. |
| Scan finds 0x68 but `[FAIL] init failed` | Sensor is seen on I2C but didn't respond to begin(). Usually a bad solder joint or magnetometer subbus glitch. Power-cycle the board and try again. |
| `[WARN] accel magnitude … far from 1g` | Either real motion (ignore) or sensor is returning stuck values. If persistent at rest → suspect the sensor. |
| Numbers stream but never change when you move the board | Sensor stuck. Same suspects as above. |
| Other devices missing in the scan | Tells you the bus is partially functional — useful for narrowing down which board has the wiring problem. |

---

## Pass criteria

- [ ] `[PASS] ICM-20948 initialized.` printed at boot
- [ ] PCA9685 and INA219 both reported FOUND in the scan (assuming both are wired)
- [ ] Accel magnitude reads ~1000 mg at rest
- [ ] Pitch/Roll respond to tilt
- [ ] Heading sweeps as the board is rotated

## Status

Pending bench test.
