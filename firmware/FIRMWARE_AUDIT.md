# Firmware Audit — legend_cutter vs Confirmed Tests

KEY

- `[ ]` — not checked yet
- `[x]` — confirmed correct, no change needed
- `[~]` — partially right, minor update made
- `[!]` — was wrong, fixed
- `[?]` — still unclear, needs a new test or decision

---

## 1. Known wrong — fix before first integrated upload

### 1a. Audio module (HIGH PRIORITY — wrong IC entirely)

`legend_cutter/audio.cpp` was written for the **DFPlayer Mini** (binary protocol,
9600 baud, `DFRobotDFPlayerMini` library, `SoftwareSerial`).

The actual hardware on the boat is the **DF1201S / DFPlayer Pro** (AT command protocol,
115200 baud, `DFRobot_DF1201S` library, `HardwareSerial(2)`).

Confirmed by: `tests/test_11_dfplayer/NOTES.md` (bringup notes document this mismatch
explicitly — three failed starts using the Mini driver before switching to the Pro driver).

**Action:** Rewrite `audio.cpp` and `audio.h` using `test_11_dfplayer.ino` as the
reference. Key differences:

| | DFPlayer Mini (current draft) | DF1201S (correct) |
|---|---|---|
| Library | `DFRobotDFPlayerMini` | `DFRobot_DF1201S` |
| Serial type | `SoftwareSerial` | `HardwareSerial(2)` |
| Baud | 9600 | 115200 |
| Play command | `dfPlayer.play(track)` | `DF1201S.playFileNum(track)` |
| Volume command | `dfPlayer.volume(vol)` | `DF1201S.setVol(vol)` |
| Init delay | 200 ms | 1000 ms before `begin()` |
| Mode setup | none | `switchFunction(MUSIC)`, 2 s settle, `setPlayMode(SINGLE)` |
| AMP control | none | `DF1201S.enableAMP()` |

Pins GPIO 25/26 are correct in `config.h` and match the test.

- [ ] Rewrite `audio.cpp`
- [ ] Rewrite `audio.h` API to match new driver
- [ ] Update any `audioPlay()` call sites in `legend_cutter.ino`

---

### 1b. iBUS rudder channel index

`config.h` defines `IBUS_CH_RUDDER 3`.

`tests/test_07_ibus_rudder` passed with `RUDDER_CHANNEL_INDEX = 0` (CH1, right-stick
horizontal, Mode 2 aileron).

**Action:** Confirm which transmitter channel the rudder stick actually outputs on your
FS-i6X setup. If it is still CH1 (index 0), change `IBUS_CH_RUDDER` to `0` in `config.h`.
If the transmitter was remapped, document the channel here and in config.h.

- [ ] Confirm actual rudder TX channel
- [ ] Update `IBUS_CH_RUDDER` in `config.h` if needed

---

### 1c. iBUS failsafe — frozen-channel detection missing

`legend_cutter/ibus.cpp` only detects signal loss as "no frames for 500 ms"
(`ibusSignalLost()`).

`tests/test_03_ibus_passthrough` verified a second condition: receiver keeps sending
frames but channels are frozen at failsafe values. In that case `ibusSignalLost()`
returns false and the boat thinks it still has RC control.

**Action:** Add frozen-channel detection to `ibusUpdate()`. Compare current channel
values against previous values on every frame; if all channels are identical for
N consecutive frames after a TX-off event, treat as lost. Threshold can be tuned.

Alternatively: confirm whether the FS-iA10B actually stops sending frames on TX off
(some receivers do, some send frozen frames). Check the test_03 NOTES for the exact
observed behavior.

- [ ] Check test_03 NOTES for exact observed FS-iA10B behavior on TX off
- [ ] Add frozen-channel detection if needed, or document why it's not needed

---

## 2. Missing modules — add from test evidence

### 2a. LED lights — no module exists

`tests/test_12_led_lights` confirmed GPIO 18 (nav), GPIO 19 (bridge/interior),
GPIO 23 (deck/flood). No corresponding module in `legend_cutter/`.

**Action:** After test_12 result is confirmed, add `lights.h` / `lights.cpp` with
`lightsBegin()` and `lightsSet()` functions. Wire into `legend_cutter.ino`
`setup()` and the state machine (e.g., running lights on in MANUAL/AUTONOMOUS,
alarm blink in FAILSAFE).

- [ ] Confirm test_12 result
- [ ] Add `lights.h` / `lights.cpp`
- [ ] Add to `legend_cutter.ino`

---

## 3. Unproven — correct architecture but no test evidence yet

These modules look structurally reasonable but have no corresponding passing test.
Do not trust them until a combined/integration test exercises them.

### 3a. Battery — `battery.cpp`

Addresses and library match test_13 expectations (`0x41`, `Adafruit_INA219`,
`setCalibration_32V_2A()`). But test_13 result is still pending.

- [ ] Confirm test_13 result (INA219 bench test)
- [ ] Check actual library version on this machine matches what the test used
- [ ] Verify `batteryGet().criticalVoltage` triggers RTH correctly in combined test

### 3b. Sonar — `sonar.cpp`

Pins match test_04 (`TRIG=27`, `ECHO=14`). Water speed constant correct (`13.4f`).
BUT: test_04 bench test was in air pointing at a flat surface. Behavior in water
through the hull is unconfirmed.

Minor: test_04 used `INPUT_PULLDOWN` for ECHO pin; `sonar.cpp` uses `INPUT`.
Probably fine but worth checking if ECHO line floats.

- [ ] Confirm test_04 bench pass (or check NOTES)
- [ ] Decide: change ECHO to `INPUT_PULLDOWN` to match test, or document why `INPUT` is fine
- [ ] Run sonar in water to confirm timing/range through hull

### 3c. GPS — `gps.cpp`

Pins match test_10b boat install (`RX=GPIO4`, `TX=GPIO17`, `9600` baud, TinyGPSPlus).
Test_10 passed. Test_10b (through-hull on boat) pass criteria still pending.

- [ ] Confirm test_10b result with antenna through hull
- [ ] Verify `gpsGet().fix` works as expected in combined test

### 3d. IMU — `imu.cpp`

Same silicon (ICM-20948), same address (`0x68`), same SparkFun library as test_09.
But the integration firmware uses DMP + FIFO fusion; test_09 used raw AGMT reads
without DMP. Different firmware paths with different failure modes.

Magnetometer calibration (`imuCalibrateMag()`) is a stub — mag reads are not
implemented yet in `imu.cpp`.

**test_22_mag_calibration — hard-iron offsets confirmed 2026-05-08:**

```c
#define MAG_OFFSET_X  -38.62f   // µT
#define MAG_OFFSET_Y   1.65f
#define MAG_OFFSET_Z  -14.85f
```

Range at calibration: X=76.35 µT, Y=51.00 µT, Z=38.70 µT. Plateau-detected
(full 360° confirmed). See `tests/test_22_mag_calibration/NOTES.md`.
Heading accuracy vs. compass bearing (Gates 4/5) still pending.

- [ ] Confirm DMP init works on this hardware (may need DMP firmware binary)
- [ ] Test heading output (imu.heading) against known orientations — use test_23
- [x] Derive magnetometer hard-iron offsets — test_22 PASS (run 2, 2026-05-08)
- [ ] Implement magnetometer reads + hard-iron correction in `imu.cpp`
- [ ] Test `imuCalibrateMag()` spin sequence with NVS storage

### 3e. Bilge — `bilge.cpp`

No test sketch exists for this at all. Logic looks reasonable but is completely
untested.

- [ ] Create `test_14_bilge` bench test: wet the sensor, confirm pump triggers
- [ ] Run bilge test before any in-water testing

### 3f. Telemetry / WiFi / WebSocket / HTTP — `telemetry.cpp`

Addressed by **test_20_wifi_leds**: WiFi AP bring-up, WebSocket at 10 Hz,
HTTP `/status` and `/led` endpoints all exercised together. LED control
confirmed end-to-end (app toggle → GPIO state) before integrating into main firmware.

- [x] Decide: WiFi telemetry IS in scope for first on-water test
- [ ] Run test_20 and confirm all 5 gates pass
- [ ] Implement remaining HTTP endpoints in `telemetry.cpp` after test_20 passes

---

## 4. Known stubs — autonomous behavior, defer to Phase 3

These are intentional placeholders. Do not try to fix them until all sensors are
proven in combined tests.

### 4a. `motorsSetAuto()` — heading hold not applied

`motors.cpp` `motorsSetAuto()` is a placeholder that ignores `headingError` and
passes neutral rudder. The PID output computed in `navigation.cpp` is never applied
to the rudder.

- [ ] After IMU heading is proven, wire `pidOut → rudderUs` in `motorsSetAuto()`
- [ ] Tune PID on the water: start with `KP=0.5`, `KI=0`, `KD=0`

### 4b. `weapons.cpp` — animation sequences incomplete

`setCIWSFire()` animation phase sequence has TODO markers. Pan/tilt basics likely
work (built on confirmed PCA9685 path) but timed sequences are not implemented.

- [ ] Test gun pan/tilt manually through RC before implementing sequences

### 4c. `navigation.cpp` — waypoint/RTH logic written but untested

PID math, waypoint sequencer, RTH trigger look correct structurally. None of it
has run on the boat.

- [ ] Defer until motors, GPS, and IMU are all proven in combined tests

---

## 5. Config constants to verify against final wiring

Go through `config.h` line-by-line once all tests are complete and wiring is final.
Flag any constant that was set by assumption rather than confirmed test.

- [ ] `IBUS_CH_THROTTLE 2` — confirm this is actually the throttle stick channel
- [ ] `IBUS_CH_RUDDER 3` — see item 1b above
- [ ] `IBUS_CH_MODE 4` — confirm SWA/SWB is on CH5 (index 4) on your FS-i6X mapping
- [ ] `DIFF_THRUST_FACTOR 0.3f` — tune on water, this is a guess
- [ ] `WAYPOINT_CAPTURE_M 3.0f` — tune on water
- [ ] `BATTERY_RTH_VOLTAGE 13.0f` — verify against your actual LiPo sag under load
- [ ] `BATTERY_ALARM_VOLTAGE 13.6f` — same
- [ ] `BILGE_DRY_DELAY_MS 5000` and `BILGE_MAX_RUN_MS 60000` — reasonable guesses, confirm with bilge test
- [ ] PCA9685 channel assignments (CH_GUN_PAN through CH_ANCHOR_AFT) — verify against actual servo wiring

---

## 6. Combined / integration tests to run before on-water

These are not written yet. Run them after individual tests are all passing.

- [ ] **iBUS + PCA9685 + motors** — RC control with all three running together (test_08 scope but within the integrated firmware)
- [ ] **iBUS + battery** — confirm failsafe triggers correctly when battery is also polling I2C
- [ ] **I2C bus under load** — PCA9685, INA219, ICM-20948 all on the same bus simultaneously; confirm no address conflicts or bus contention (test_09 NOTES documented that unpowered I2C devices can drag the bus)
- [ ] **GPS + IMU together** — heading + position at the same time
- [ ] **Sonar + everything else** — `pulseIn()` is blocking; confirm it doesn't disrupt iBUS frame parsing at 50 Hz
- [ ] **DFPlayer + ESCs** — audio over serial while motors are running; ESC noise on power rail can corrupt serial; confirm shielding/filtering is adequate
- [ ] **WiFi + everything** — `ADC2` is unavailable when WiFi is active; `bilge.cpp` uses ADC1 pins (32, 33) which should be fine, but verify

---

*Last updated: 2026-05-08. Re-review after combined tests complete.*
