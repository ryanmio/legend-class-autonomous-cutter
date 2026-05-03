# test_17_manual_sailing — Full Manual Control Loop

Closes the manual sailing loop end-to-end: **transmitter sticks → ESP32 (iBUS) → PCA9685 → both ESCs + rudder servo**, with the production differential-thrust mixing math and a reverse-interlock safety.

This is the integration test that proves you can sail the boat by stick. It supersedes the individual-channel tests by exercising rudder + both motors + the mixing logic + failsafe simultaneously.

Builds on:
- `test_03_ibus_passthrough` (iBUS parsing + channel-freeze failsafe)
- `test_07_ibus_rudder` / `test_15_rudder_max_deflection` (rudder limits)
- `test_08_ibus_esc` (ESC arming, throttle gating, forward cap)

---

## Stick scheme

Mode-2 with center-sprung throttle. The reverse-interlock is the new piece.

| Stick | iBUS ch | Role |
|---|---|---|
| Right stick H | CH1 (idx 0) | Rudder |
| Right stick V | CH2 (idx 1) | **Reverse command** (down = more reverse) |
| Left stick V  | CH3 (idx 2) | **Forward throttle** + reverse-enable interlock |

### Reverse interlock

Reverse only engages when **both** are true:

1. Left stick is pushed all the way down (≤ `LEFT_STICK_DOWN_US`, default 1100 µs).
2. Right stick V is below neutral (CH2 < 1500 - dead band).

Right-stick down by itself does nothing. Left-stick down with right stick centered does nothing. You must commit with both sticks. This prevents bumping the right stick mid-cruise from kicking the boat into reverse.

In forward mode (left stick > 1530), the right stick V is ignored entirely — the operator can rest hands without spurious reverse commands.

---

## What this test checks

| Gate | What |
|---|---|
| 1/5 | iBUS acquired AND all sticks at safe neutral → ARMED |
| 2/5 | Rudder swept LEFT past `RUDDER_LEFT_THRESHOLD_US` |
| 3/5 | Rudder swept RIGHT past `RUDDER_RIGHT_THRESHOLD_US` |
| 4/5 | Forward throttle advanced WITH rudder offset → port/stbd ESC outputs differ by ≥ `DIFF_MIN_SPLIT_US` (proves differential thrust mixing actually splits) |
| 5/5 | Reverse interlock — Phase A (right-stick down with left centered → motors STAY at 1500) AND Phase B (both sticks down → motors reverse) |

Channel-freeze failsafe is active in every state once signal is acquired. Killing the TX during any gate forces all three outputs to neutral and disarms back to the safe-neutral check.

After all gates pass and sticks return to neutral, the sketch enters **LIVE** mode for free-form sailing.

---

## Safety design

- **Props OFF, or boat firmly secured.** Same rule as test_08.
- **Forward output cap** at `MAX_FWD_US` (1800 µs ≈ 60% forward). The stick can go to 2000, the ESC never sees more than 1800. The cap is on the µs sent to the ESC, not on the iBUS reading — the stick value is preserved for telemetry/debug.
- **Reverse output cap** at `MIN_REV_US` (1200 µs ≈ 60% reverse).
- **Rudder clamped** to `RUDDER_MIN_US..RUDDER_MAX_US` (1330..1670, from test_15). Past those bounds the dogbone linkage flips center and jams.
- **Arm-on-neutral.** Motors stay at 1500 until the operator centers every stick *after* signal is acquired.
- **Channel-freeze failsafe** within 500 ms — Flysky receivers keep streaming frames after TX-off with frozen values, so the test compares each frame against the previous one and trips on no-change for 500 ms. Outputs all forced to 1500 / rudder neutral; sketch disarms.
- **No-frame failsafe** within 500 ms — covers RX wire fall-off (less likely than TX-off but cheap to keep).
- **Re-arm on recovery.** Even after signal returns, motors stay at 1500 until the operator brings every stick to neutral.
- **Drive authorization gate.** The motors are clamped to 1500 in software whenever the test state is `WAIT_SIGNAL`, `WAIT_SAFE_NEUTRAL`, or failsafe — the rudder gates (states 2 and 3) intentionally let the rudder *servo* move while keeping the *motors* off, so the operator can sweep without spinning props.

---

## Hardware required

Same as test_08:
- ESP32 with iBUS divider on GPIO 16 (1 kΩ + 2 kΩ to GND).
- PCA9685 powered (logic 3.3 V, V+ rail at 6 V for the rudder servo).
- Port ESC signal → PCA9685 ch0; starboard → ch1; rudder → ch2.
- Both ESCs on battery; common ground with ESP32 and PCA9685.
- Flysky FS-iA10B receiver bound to FS-i6X (or whatever TX is bound).
- **PROPS REMOVED** for the first run, or boat clamped to the bench.

---

## Tunables in the sketch

Top of `test_17_manual_sailing.ino`:

| Constant | Default | Purpose |
|---|---|---|
| `RUDDER_CHANNEL_INDEX` | `0` | CH1, right-stick H |
| `REVERSE_CHANNEL_INDEX` | `1` | CH2, right-stick V (down = reverse) |
| `THROTTLE_CHANNEL_INDEX` | `2` | CH3, left-stick V (forward + interlock) |
| `RUDDER_MIN_US` / `MAX_US` | `1330` / `1670` | from test_15 |
| `MAX_FWD_US` | `1800` | forward output cap |
| `MIN_REV_US` | `1200` | reverse output cap |
| `LEFT_STICK_DOWN_US` | `1100` | left stick "all the way down" interlock threshold |
| `REVERSE_DEADBAND_US` | `30` | right-stick-V must be this far below 1500 to count as reverse intent |
| `NEUTRAL_DEADBAND_US` | `30` | ±this around 1500 = centered |
| `DIFF_THRUST_FACTOR` | `0.3` | mirrors `motors.cpp` |
| `RUDDER_LEFT_THRESHOLD_US` | `1400` | rudder must reach this for L sweep gate |
| `RUDDER_RIGHT_THRESHOLD_US` | `1600` | RIGHT sweep gate |
| `FWD_ADVANCE_THRESHOLD_US` | `1700` | left-stick must reach this with rudder offset for diff gate |
| `RUDDER_OFFSET_FOR_DIFF_US` | `50` | \|rudder - 1500\| must exceed this during fwd gate |
| `DIFF_MIN_SPLIT_US` | `30` | \|port - stbd\| must exceed this to count as "split" |
| `PHASE_A_HOLD_MS` | `1500` | how long operator must hold the interlock-test pose |

---

## Procedure

1. **Props off** (or boat secured to bench / rails).
2. Open `test_17_manual_sailing/test_17_manual_sailing.ino`. Board: ESP32 Dev Module.
3. Upload. Serial Monitor at **115200**.
4. Power up ESCs (battery on). They will beep their arming chirp during the 3 s neutral-hold.
5. Turn on transmitter. Hands off (sticks at rest).
6. Follow on-screen steps:
   - Sweep rudder left, then right.
   - Hold rudder off-center, advance forward throttle past 1700 µs. Sketch confirms PORT and STBD outputs split.
   - Return throttle to neutral.
   - **Phase A:** with left stick centered, push right stick down. Hold ~1.5 s. Motors must stay silent. Sketch logs `[OK] interlock holds`.
   - **Phase B:** push left stick all the way down AND keep right stick down. Motors should reverse. Sketch logs PASS 5/5.
   - Return all sticks to neutral. Sketch announces ALL GATES PASSED and enters LIVE.
7. In LIVE mode, sail the boat freely on the bench. Cut TX power once to verify failsafe stops everything within ~500 ms.

---

## Failure modes the sketch surfaces

| Symptom | What it means |
|---|---|
| `[WARN] checksum mismatch` (rate-limited) | iBUS wiring noise. Same caveat as test_15 — see test_16 for the systematic measurement. |
| `[FAIL] interlock broken: right-stick down with left centered drove throttle to N µs.` | `computeThrottleUs()` is wrong. The whole point of the interlock failed. Halt and fix before any further testing. |
| `>>> FAILSAFE — channels frozen (TX off?)` | Normal Flysky failsafe path. All outputs forced to 1500. |
| `>>> FAILSAFE — no iBUS frames (RX disconnected?)` | Receiver power / signal wire fell off. |
| `[FAIL] PCA9685 not found` | I²C broken — re-run test_06 (and check VCC at the breakout, per project memory). |

---

## Pass criteria

- [ ] `PASS (1/5)` — sticks at safe neutral, ARMED
- [ ] `PASS (2/5)` — rudder LEFT sweep
- [ ] `PASS (3/5)` — rudder RIGHT sweep
- [ ] `PASS (4/5)` — differential thrust split observed (port ≠ stbd by ≥ `DIFF_MIN_SPLIT_US`)
- [ ] `PASS (5/5)` — reverse interlock works in both directions (Phase A holds, Phase B engages)
- [ ] (visual) Both motors spin during forward gate; outside motor visibly faster than inside
- [ ] (visual) Both motors reverse during Phase B
- [ ] Failsafe stops motors and centers rudder within ~500 ms of TX off
- [ ] Re-arm requires sticks at neutral (motors do NOT immediately resume on signal recovery)

---

## Result

_pending — fill in on first run._
