# test_17_manual_sailing — Full Manual Control Loop

Closes the manual sailing loop end-to-end: **transmitter sticks → ESP32 (iBUS) → PCA9685 → both ESCs + rudder servo**, with the production differential-thrust mixing math and a reverse-interlock safety.

This is the integration test that proves you can sail the boat by stick. It supersedes the individual-channel tests by exercising rudder + both motors + the mixing logic + failsafe simultaneously.

Builds on:
- `test_03_ibus_passthrough` (iBUS parsing + channel-freeze failsafe)
- `test_07_ibus_rudder` / `test_15_rudder_max_deflection` (rudder limits)
- `test_08_ibus_esc` (ESC arming, throttle gating, forward cap)

---

## Stick scheme

Mode-2. Throttle stick rests at the **bottom** (1000 µs) — not center-sprung.
The right stick is spring-centered on both axes.

| Stick | iBUS ch | Role |
|---|---|---|
| Right stick H | CH1 (idx 0) | Rudder (full stick scaled to `RUDDER_MIN_US..RUDDER_MAX_US`) |
| Right stick V | CH2 (idx 1) | **Reverse command** (down = more reverse) |
| Left stick V  | CH3 (idx 2) | **Forward throttle** (any lift off idle = forward) + reverse-enable interlock |

### Throttle behavior

- Stick at bottom (≤ `THROTTLE_IDLE_MAX_US`, default 1100) → **idle**, motors stopped.
- Stick lifted off idle → forward, scaled linearly into `[1500..MAX_FWD_US]`. Stick at top (2000) → output 1800 µs (forward cap).
- Operator's natural rest = stick at bottom = motors stopped. So power-on with hands off the stick = boat doesn't move.

### Reverse interlock

Reverse only engages when **both** are true:

1. Throttle stick is at idle (≤ `THROTTLE_IDLE_MAX_US`, i.e. resting at the bottom).
2. Right stick V is below neutral (CH2 < 1500 - dead band).

While throttle is forward (stick lifted), the right stick V is **ignored** — interlock blocks reverse. So you can't slam the boat from forward to reverse with a single stick movement; you have to first drop the throttle to idle, then engage reverse.

### Smoothing

Throttle and rudder targets are slew-rate limited. A stick slammed to its
extreme ramps to it over `THROTTLE_SLEW_US_PER_SEC` / `RUDDER_SLEW_US_PER_SEC`
respectively (default: 400 / 1000 µs/sec, so full forward in ~0.75 s and full
rudder sweep in ~0.34 s). Failsafe entry **bypasses** slew — outputs snap to
1500 immediately for safety.

---

## What this test checks

| Gate | What |
|---|---|
| 1/5 | iBUS acquired AND all sticks at safe neutral → ARMED |
| 2/5 | Rudder swept LEFT past `RUDDER_LEFT_THRESHOLD_US` |
| 3/5 | Rudder swept RIGHT past `RUDDER_RIGHT_THRESHOLD_US` |
| 4/5 | Forward throttle advanced WITH rudder offset → port/stbd ESC outputs differ by ≥ `DIFF_MIN_SPLIT_US` (proves differential thrust mixing actually splits) |
| 5/5 | Reverse interlock — Phase A (left lifted forward + right-stick down → throttle STAYS forward, no reverse) AND Phase B (left at idle + right down → motors reverse) |

Channel-freeze failsafe is active in every state once signal is acquired. Killing the TX during any gate forces all three outputs to neutral and disarms back to the safe-neutral check.

After all gates pass and sticks return to neutral, the sketch enters **LIVE** mode for free-form sailing.

---

## Safety design

- **Props OFF, or boat firmly secured.** Same rule as test_08.
- **Forward output cap** at `MAX_FWD_US` (1800 µs ≈ 60% forward). The stick can go to 2000, the ESC never sees more than 1800. The cap is on the µs sent to the ESC, not on the iBUS reading — the stick value is preserved for telemetry/debug.
- **Reverse output cap** at `MIN_REV_US` (1200 µs ≈ 60% reverse).
- **Rudder scaled** so full stick range (1000..2000) maps linearly to `RUDDER_MIN_US..RUDDER_MAX_US` (1330..1670, from test_15). Stick at full extreme = rudder at hardware limit. No "dead zone" past partial deflection.
- **Slew-rate limited** throttle and rudder so quick stick jerks ramp instead of slamming. Tunable via `THROTTLE_SLEW_US_PER_SEC` / `RUDDER_SLEW_US_PER_SEC`. Failsafe bypasses slew (immediate stop).
- **Arm-on-safe-neutral.** Motors stay at 1500 until the throttle stick is at idle AND the right stick + rudder are centered, *after* signal is acquired.
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
| `THROTTLE_IDLE_MAX_US` | `1100` | left stick at/below this = idle (rest position). Above = forward (scaled). At/below also enables reverse via right-stick V. |
| `THROTTLE_SLEW_US_PER_SEC` | `400` | throttle slew limit (1500→1900 over 1 s) |
| `RUDDER_SLEW_US_PER_SEC` | `1000` | rudder slew limit (full sweep ~0.34 s) |
| `REVERSE_DEADBAND_US` | `30` | right-stick-V must be this far below 1500 to count as reverse intent |
| `NEUTRAL_DEADBAND_US` | `30` | rudder/right-stick-V within 1500±this = centered |
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
5. Turn on transmitter. Throttle stick at the **bottom** (idle); right stick at rest (centered).
6. Follow on-screen steps:
   - Sweep rudder left, then right.
   - Hold rudder off-center, advance forward throttle past 1700 µs. Sketch confirms PORT and STBD outputs split.
   - **Phase A:** keep left stick lifted (forward) AND pull right stick down. Hold ~1.5 s. Throttle must STAY forward — interlock blocks reverse while throttle is forward. Sketch logs `[OK] interlock holds`.
   - **Phase B:** drop left stick to idle (bottom) AND keep right stick down. Motors should now reverse. Sketch logs PASS 5/5.
   - Return all sticks to safe neutral (throttle at bottom, right stick centered, rudder centered). Sketch announces ALL GATES PASSED and enters LIVE.
7. In LIVE mode, sail the boat freely on the bench. Cut TX power once to verify failsafe stops everything within ~500 ms.

---

## Failure modes the sketch surfaces

| Symptom | What it means |
|---|---|
| `[WARN] checksum mismatch` (rate-limited) | iBUS wiring noise. Same caveat as test_15 — see test_16 for the systematic measurement. |
| `[FAIL] interlock broken: forward + right-down drove throttle to N µs (reverse).` | `computeThrottleUs()` is wrong — right-stick-down flipped throttle to reverse while forward was commanded. Halt and fix before any further testing. |
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

### v1 — 2026-05-03 PASS (initial)

Five gates verified in firmware. Operator-reported usability issues drove a v2:

- Throttle was 1500-centered in the original logic — operator's TX has the throttle stick resting at the bottom, so motors didn't engage until the stick was pushed past center.
- Re-arm required all sticks at 1500 — impossible for a non-center-sprung throttle.
- Rudder was clamped (not scaled) so the cap kicked in at ~30% stick deflection, making the remaining 70% of stick travel feel dead.
- Stick movement was 1:1 instantaneous — quick stick jerks slammed the motors.

### v2 — pending (2026-05-04)

Changes:
- Throttle stick: bottom = idle, lifted = forward (scaled to `MAX_FWD_US`). New `THROTTLE_IDLE_MAX_US = 1100` is the idle threshold.
- Re-arm checks throttle at idle, not throttle at center.
- Rudder scales full stick range to `RUDDER_MIN_US..RUDDER_MAX_US` (was clamped before).
- Slew-rate limiter on throttle (`THROTTLE_SLEW_US_PER_SEC = 400`) and rudder (`RUDDER_SLEW_US_PER_SEC = 1000`). Bypassed on failsafe entry.
- Phase A reframed: forward throttle commanded + right stick down → interlock keeps throttle forward (was: left centered + right down → output stays at 1500).
