# test_03_ibus_passthrough — Flysky iBUS Parsing

Bench test. Confirm the Flysky receiver is wired correctly and the ESP32 can receive, parse, and act on a live iBUS signal — including detecting failsafe.

---

## Objective

| # | What we're checking |
|---|---------------------|
| 1 | iBUS frames arrive on UART1 RX (GPIO 16) with valid checksums |
| 2 | Channel values respond to stick movement |
| 3 | Failsafe is detected when the transmitter is turned off |

---

## How failsafe is detected

Flysky receivers (FS-iA6B and similar) **do not stop sending frames** when they lose the transmitter signal. They keep streaming valid, checksum-correct iBUS frames at the normal rate — the channel values just freeze at the failsafe presets configured on the transmitter.

So the sketch detects failsafe by watching for **channel values to stop changing**, not by watching for frames to stop arriving:

- If all 10 channels hold identical values for 500 ms → failsafe.
- A secondary 500 ms no-frame timeout is also kept, in case the receiver is physically disconnected.

---

## Hardware required

- ESP32 DevKit (USB-powered for bench)
- Flysky receiver (e.g. FS-iA6B) bound to transmitter
- Voltage divider on iBUS line (iBUS is 5V; GPIO 16 is 3.3V max)
- PCA9685 wired (sketch arms ESCs at neutral on boot and drives passthrough during the test)

**Library required:** `Adafruit PWM Servo Driver Library` — install from Arduino IDE Library Manager. The sketch will not compile without it.

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
| ESP32 GPIO 21 | PCA9685 SDA |
| ESP32 GPIO 22 | PCA9685 SCL |

---

## Flash and run

1. Open Arduino IDE.
2. Open `test_03_ibus_passthrough/test_03_ibus_passthrough.ino`.
3. **Board:** `ESP32 Dev Module`
4. **Port:** your ESP32's COM/tty port.
5. Click **Upload**.
6. Open **Tools → Serial Monitor**, baud **115200**.
7. Follow the on-screen steps.

---

## Expected output

The sketch is a 3-stage state machine. It prints one PASS line per stage and goes silent once all three pass. While waiting in a stage, it prints one quiet hint every 10 s.

```
========================================
  test_03_ibus_passthrough
========================================
[INFO] PCA9685 found — arming ESCs at 1500 µs (3 s delay)...
[INFO] ESCs armed.

Step 1: turn on your transmitter.
----------------------------------------

PASS (1/3): iBUS signal acquired, checksums OK.
Step 2: move any stick on your transmitter.
----------------------------------------

PASS (2/3): channel values respond to stick input.
Step 3: TURN OFF your transmitter to test failsafe.
----------------------------------------

PASS (3/3): failsafe detected (channels frozen).

========================================
  ALL TESTS PASSED — iBUS chain verified
========================================
```

After the final banner the sketch holds outputs at neutral and is silent. Press **EN/RST** on the ESP32 to re-run.

---

## Troubleshooting

- **Stuck at Step 1:** check voltage divider, confirm receiver is bound and powered, check GPIO 16 wiring.
- **`[WARN] checksum mismatch` repeating:** noise on the iBUS line — check resistor values and shorten the wire.
- **Step 3 never passes after TX off:** confirm the transmitter has failsafe values configured (FS-i6 menu → Failsafe), not "no signal" / hold-last. With hold-last the channels truly never change post-loss either, but most receivers send the same exact frozen values, which the freeze check still catches within 500 ms.

---

## Pass criteria

- [x] `PASS (1/3)` — frames arriving with valid checksums
- [x] `PASS (2/3)` — channel values respond to stick input
- [x] `PASS (3/3)` — failsafe triggers within 500 ms of TX off

## Result — 2026-04-25

```
PASS (1/3): iBUS signal acquired, checksums OK.
PASS (2/3): channel values respond to stick input.
PASS (3/3): failsafe detected (channels frozen).

========================================
  ALL TESTS PASSED — iBUS chain verified
========================================
```

**Status: PASS**
