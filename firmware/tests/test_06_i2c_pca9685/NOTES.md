# test_06_i2c_pca9685 — ESP32 + PCA9685 Bringup

Bench test. Confirm the ESP32 can talk to the PCA9685 over I2C. No servos or external power needed — USB powers everything.

---

## Wiring (bench)

| From | To | Notes |
|------|----|-------|
| ESP32 3.3V | PCA9685 VCC | Logic power — **not** 5V |
| ESP32 GND | PCA9685 GND | |
| ESP32 GPIO 21 | PCA9685 SDA | |
| ESP32 GPIO 22 | PCA9685 SCL | |

Leave PCA9685 **V+** disconnected (no servos on the bench).

> **PCA9685 address jumpers:** A0/A1/A2 must all be open (unsoldered) to get address 0x40.

---

## Flash and run

1. Open Arduino IDE.
2. Open `test_06_i2c_pca9685/test_06_i2c_scan.ino`.
3. **Board:** `ESP32 Dev Module`
4. **Port:** select your ESP32's COM/tty port.
5. Click **Upload**.
6. Open **Tools → Serial Monitor**, baud **115200**.
7. Press **EN (reset)** on the ESP32.

**Expected output:**
```
--- I2C Bus Scan ---
SDA=GPIO21  SCL=GPIO22

  Found device at 0x40  <-- PCA9685 (servo driver)

1 device(s) found.
PASS: PCA9685 at 0x40 confirmed.
```

**If nothing shows up:**
- Swap SDA/SCL wires and retry.
- Confirm PCA9685 VCC is on 3.3V (not 5V).
- Check all four wires are seated on the correct pins.

---

## Pass criteria

- [x] Serial monitor shows `PASS: PCA9685 at 0x40 confirmed.`

## Result — 2026-04-19

```
--- I2C Bus Scan ---
SDA=GPIO21  SCL=GPIO22
  Found device at 0x40  <-- PCA9685 (servo driver)
  Found device at 0x70
2 device(s) found.
PASS: PCA9685 at 0x40 confirmed.
```

0x70 is the PCA9685 all-call broadcast address — normal, not a second device.

**Status: PASS**
