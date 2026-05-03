# test_14_systems_check — Post-Waterproofing Bringup

Single-pass health check that verifies every critical subsystem is wired correctly and returning sensible values **without moving any servo, ESC, or other actuator**. Safe to run on a fully assembled boat sitting on the bench while you reconnect things one by one.

Use this any time you've opened the hull — waterproofing, antenna re-route, swapping the ESP32, or anything that could have disturbed a wire.

---

## Objective

| Gate | Subsystem | What we're checking |
|------|-----------|---------------------|
| 1/6 | I²C bus scan | All four expected addresses present (0x40 PCA9685, 0x41 INA219, 0x68 ICM-20948, plus the 0x70 PCA all-call broadcast) |
| 2/6 | PCA9685 | ACKs `setPWMFreq(50)` + MODE1 readback. **No servo movement.** |
| 3/6 | INA219 | Bus voltage in 4S LiPo range (10.0–17.5 V) |
| 4/6 | ICM-20948 IMU | `begin()` succeeds and accel magnitude ≈ 1 g |
| 5/6 | GPS | Valid NMEA within ~5 s — fix NOT required (perfect for indoor reassembly) |
| 6/6 | DF1201S DFPlayer Pro | Module ACKs `begin()` over UART. **No audio plays.** |

After all six gates the sketch streams a 1 Hz health line so you can watch for dropouts as you wiggle harnesses, close hatches, etc. The live line also re-pokes I²C every second, so a wire that comes loose post-init shows up immediately as `i2c[P-M]` (with the missing letter dropped).

---

## Hardware required

- Fully assembled boat (or any subset; failed gates print `SKIP` for missing devices)
- Laptop with USB to the ESP32
- 4S LiPo connected (so INA219 has something to measure)
- BN-220 GPS antenna with at least minimal sky exposure (or just don't worry about the GPS — fix isn't required to PASS)

---

## Libraries required (Arduino IDE → Library Manager)

- **Adafruit PWM Servo Driver Library** (PCA9685)
- **Adafruit INA219**
- **SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library**
- **TinyGPSPlus** by Mikal Hart
- **DFRobot_DF1201S** by DFRobot

---

## Wiring (boat config — matches `legend_cutter/config.h`)

| Bus / device | ESP32 pin | Notes |
|---|---|---|
| I²C SDA | GPIO 21 | Shared by PCA9685, INA219, ICM-20948 |
| I²C SCL | GPIO 22 | |
| GPS TX (**white**) | GPIO 17 | UART2 RX. White=TX on this BN-220 batch — see test_10b NOTES |
| GPS RX (**green**) | GPIO 4  | UART2 TX, optional |
| DF1201S TX | GPIO 25 | UART1 RX |
| DF1201S RX | GPIO 26 | UART1 TX |

---

## Failure modes the sketch surfaces

| Symptom | Likely cause |
|---|---|
| `1/6 FAIL — at least one I2C device missing` | Power at the missing breakout pad, or a Wago flaking on the SDA/SCL line. Multimeter VCC at the device pad first. |
| `2/6 FAIL — PCA9685 ACKed but no MODE1 byte` | I²C is present but the chip is partially up — usually a brownout. Check 3.3 V to the PCA9685. |
| `3/6 FAIL — voltage out of 4S range` | Battery disconnected, VIN+/VIN- swapped on the INA219, or a pack with a damaged cell. |
| `4/6 FAIL — IMU init OK but no sample` | Mag init timeout — power-cycle and retry. Persistent failure usually means a bad solder joint on the ICM. |
| `5/6 FAIL — bytes arriving but no NMEA` | First suspect: wire-color reversal (white must go to GPIO 17). Then: power at the BN-220 pad, then baud. |
| `6/6 FAIL — DF1201S no response` | VCC at DFPlayer pad, GND tie, and the ESP32-TX(26) → DFP-RX / ESP32-RX(25) ← DFP-TX crossover. Library must be `DFRobot_DF1201S`, not the Mini. |

---

## Pass criteria

- [x] All 6 gates report PASS in the SUMMARY block
- [x] Live 1 Hz line shows stable values for ≥30 s with no `i2c[--M]`-style dropouts

## Result — 2026-05-03

```
============================================
  SUMMARY
============================================
  1/6 I2C scan      : PASS
  2/6 PCA9685       : PASS
  3/6 INA219 volts  : PASS
  4/6 ICM-20948 IMU : PASS
  5/6 GPS NMEA      : PASS
  6/6 DFPlayer Pro  : PASS

  ALL SYSTEMS NOMINAL — streaming 1 Hz health line.
============================================
```

Confirms post-waterproofing reassembly is electrically sound. GPS step passed after the white=TX / green=RX fix landed in `legend_cutter/config.h` — see test_10b NOTES for the root cause.

**Status: PASS**
