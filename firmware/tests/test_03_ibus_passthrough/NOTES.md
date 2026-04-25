# test_03_ibus_passthrough — Flysky iBUS Parsing

Confirm the Flysky receiver is wired correctly and the ESP32 can receive, parse, and act on a live iBUS signal.

---

## Objective

| # | What we're checking |
|---|---------------------|
| 1 | iBUS frames arrive on UART1 RX (GPIO 16) |
| 2 | Frames pass the checksum — data is not corrupted |
| 3 | All channel values are in range (1000–2000 µs) and respond to stick movement |
| 4 | Failsafe triggers within 500 ms when the transmitter is turned off |

---

## Hardware required

- ESP32 DevKit (USB-powered for bench)
- Flysky receiver (e.g. FS-iA6B) bound to transmitter
- Voltage divider on iBUS line (iBUS is 5V; GPIO 16 is 3.3V max)
- PCA9685 optional — sketch auto-detects it; servos/ESCs will move if present

---

## Voltage divider (required)

iBUS signal is 5V. Wire a resistor divider before GPIO 16:

```
Receiver iBUS pin → [1kΩ] → GPIO 16
                              ↓
                            [2kΩ]
                              ↓
                             GND
```

Without this, you will damage the ESP32 GPIO over time and may get corrupted reads.

---

## Wiring

| From | To |
|------|----|
| Receiver iBUS (signal) | 1kΩ resistor → GPIO 16 (with 2kΩ to GND as above) |
| Receiver VCC (5V) | ESP32 VIN or 5V bench supply |
| Receiver GND | ESP32 GND |
| ESP32 GPIO 21 | PCA9685 SDA (if connected) |
| ESP32 GPIO 22 | PCA9685 SCL (if connected) |

---

## Flash and run

1. Open Arduino IDE.
2. Open `test_03_ibus_passthrough/test_03_ibus_passthrough.ino`.
3. **Board:** `ESP32 Dev Module`
4. **Port:** your ESP32's COM/tty port.
5. Click **Upload**.
6. Open **Tools → Serial Monitor**, baud **115200**.
7. Turn on your transmitter.

---

## What to tell the model (Serial Monitor output)

The sketch walks you through three pass gates. Report what you see at each stage:

**Stage 1 — signal acquired:**
```
>>> SIGNAL ACQUIRED <<<
PASS (1/3): iBUS frames arriving and checksum OK.
```
If this never appears: check voltage divider, confirm receiver is bound and powered, check GPIO 16.

**Stage 2 — 50 clean frames (auto-prints):**
```
PASS (2/3): 50 consecutive frames received with valid checksums.
  Values should be ~1500 at stick center, ~1000 at min, ~2000 at max.
```
Move sticks and confirm CH values change in the printout.

**Stage 3 — turn off transmitter:**
```
>>> FAILSAFE TRIGGERED (no signal for 500 ms) <<<
PASS (3/3): Failsafe working.
```

If you see `[WARN] Checksum mismatch` repeatedly: noise on the iBUS wire — check the voltage divider resistor values and wire length.

---

## Pass criteria

- [ ] `PASS (1/3)` — frames arriving and checksum OK
- [ ] `PASS (2/3)` — 50 consecutive clean frames; stick movement changes values
- [ ] `PASS (3/3)` — failsafe triggers within 500 ms of TX off

## Status

Pending bench test.
