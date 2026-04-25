# test_07_ibus_rudder — Rudder Servo via iBUS Passthrough

Closes the manual control loop: **transmitter stick → ESP32 (iBUS) → PCA9685 → rudder servo**. This is the core manual-control path for the boat.

Builds on `test_03_ibus_passthrough` (iBUS parsing verified) and `test_06_i2c_pca9685` (I2C/PCA9685 verified).

---

## Objective

| # | What we're checking |
|---|---------------------|
| 1 | iBUS signal acquired, rudder holding center at startup |
| 2 | Stick pushed FULL LEFT moves rudder to the left extreme |
| 3 | Stick pushed FULL RIGHT moves rudder to the right extreme |
| 4 | (visual) Response is smooth, proportional, correctly oriented, and the linkage doesn't bind |

The first three are auto-verified in firmware. The fourth is a visual check by the operator after the range test passes.

---

## Hardware required

- ESP32 with the wiring from test_03 (iBUS divider on GPIO 16, I2C on 21/22)
- PCA9685 powered (logic on 3.3V, **V+ rail at 6V** for the rudder servo)
- Rudder servo connected to **PCA9685 ch2** (signal + 6V V+ + GND)
- ESCs may be left wired to ch0/ch1 — the sketch holds them at 1500 µs throughout, so motors stay stopped
- Flysky receiver bound and powered

If the rudder is already installed in the hull with the dogbone linkage, run the test with the linkage connected so you can verify free movement at the extremes.

---

## Wiring (delta from test_03)

| From | To |
|------|----|
| Rudder servo signal | PCA9685 ch2 signal (yellow) |
| Rudder servo V+ | PCA9685 V+ rail (red, 6V) |
| Rudder servo GND | PCA9685 GND rail (black/brown) |

Everything else (iBUS, I2C, ESP32 power) is unchanged from test_03.

---

## Tunables in the sketch

Top of `test_07_ibus_rudder.ino`:

| Constant | Default | What it does |
|----------|---------|--------------|
| `RUDDER_CHANNEL_INDEX` | `0` | iBUS channel index (0-based). `0` = CH1 = right-stick horizontal (Mode-2 aileron). Use `3` for CH4 / left-stick yaw. |
| `RUDDER_PCA_CHANNEL` | `2` | PCA9685 output channel. |
| `RUDDER_REVERSE` | `false` | Set `true` to reverse direction in firmware (instead of flipping the horn). |
| `LEFT_PASS_US` | `1100` | Stick µs ≤ this counts as "full left" for PASS 2/3. |
| `RIGHT_PASS_US` | `1900` | Stick µs ≥ this counts as "full right" for PASS 3/3. |

If the linkage binds before the stick reaches full deflection, narrow the firmware travel by post-mapping the rudder µs (TODO — not yet implemented; for now, mechanical fix: shorten the servo arm or pushrod throw).

---

## Flash and run

1. Open `test_07_ibus_rudder/test_07_ibus_rudder.ino` in Arduino IDE.
2. **Board:** `ESP32 Dev Module`. **Port:** your ESP32's port.
3. Upload.
4. Open Serial Monitor at **115200**.
5. Center the rudder stick on the transmitter, then turn the transmitter on.
6. Follow the on-screen steps.

---

## Expected output

```
========================================
  test_07_ibus_rudder
========================================
Rudder stick: CH4 (iBUS index 3)
Rudder servo: PCA9685 ch2  (reverse=false)

[INFO] PCA9685 found — arming ESCs at 1500 µs (3 s delay)...
[INFO] ESCs armed. Rudder centered.

Step 1: turn on your transmitter and center the rudder stick.
----------------------------------------

PASS (1/3): iBUS signal acquired, rudder centered.
  Rudder channel reads 1500 µs.
Step 2: push the rudder stick FULL LEFT.
----------------------------------------

PASS (2/3): full LEFT reached (CH4=1004 µs).
Step 3: push the rudder stick FULL RIGHT.
----------------------------------------

PASS (3/3): full RIGHT reached (CH4=1996 µs).

========================================
  RANGE TEST PASSED
========================================
Now visually verify on the bench / in the hull:
  [ ] response is smooth and proportional
  [ ] direction is correct (left stick = left rudder)
  [ ] linkage moves freely at extremes (no binding)
```

After the banner the rudder stays live — keep moving the stick to walk through the visual checks.

---

## Troubleshooting

- **No PASS 1/3 — `[FAIL] PCA9685 not found`:** Re-run test_06; check I2C wiring.
- **No PASS 1/3 — sketch is silent:** iBUS not arriving. Re-run test_03 first.
- **Servo moves but direction is wrong:** Set `RUDDER_REVERSE = true` and re-flash, or rotate the servo horn 180° on the splines (mechanical fix is preferred — keeps firmware mapping clean).
- **Servo binds before full stick deflection:** Either shorten the throw on the servo arm (mechanical), or — once we add a travel-limit feature to the sketch — clamp the output µs.
- **Servo jitters at extremes:** Likely fighting the linkage at the bind point; back off the servo arm length or check for a tight pushrod hole.
- **Wrong stick controls the rudder:** Change `RUDDER_CHANNEL_INDEX`. Defaults to CH1 (right-stick horizontal, Mode-2 aileron) for drone-pilot muscle memory. For left-stick yaw, set to `3` (CH4).

---

## Pass criteria

- [x] `PASS (1/3)` — signal acquired, rudder centered
- [x] `PASS (2/3)` — full LEFT reached
- [x] `PASS (3/3)` — full RIGHT reached
- [x] Smooth, proportional response (visual, servo only)
- [x] Correct orientation on right-stick horizontal (CH1)
- [ ] No binding at extremes — **deferred:** retest with rudder + dogbone linkage installed in hull

## Result — 2026-04-25

Bench test PASS with servo only (no rudder shaft / linkage attached, just the servo pivot). Right-stick horizontal correctly drives the servo full-range left and right.

**Re-run required** once the rudder and dogbone linkage are installed in the hull, to confirm the linkage moves freely through full deflection without binding at the joints or bulkhead pass-throughs.

**Status: PASS (servo-only). Linkage check pending.**
