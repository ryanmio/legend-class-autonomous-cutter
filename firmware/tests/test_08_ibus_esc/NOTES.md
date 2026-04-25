# test_08_ibus_esc — ESC / Motor Spin via iBUS Passthrough

Closes the throttle loop: **transmitter throttle stick → ESP32 (iBUS) → PCA9685 → both ESCs → motors**.

Builds on:
- `test_02_esc_motor` (PCA→ESC→motor pathway, no transmitter — auto-ramp)
- `test_03_ibus_passthrough` (iBUS parsing + failsafe)
- `test_07_ibus_rudder` (iBUS-driven servo passthrough)

This is the first test where motors actually spin under transmitter control. **PROPS OFF or boat secured.**

---

## Objective

| # | What we're checking |
|---|---------------------|
| 1 | iBUS signal acquired AND throttle stick at neutral → ESCs ARMED |
| 2 | Throttle stick advanced past 1700 µs spins both motors |
| 3 | Throttle returned to neutral cleanly stops both motors |
| 4 | (visual) Both motors spun, no excess heat/smell, stop is clean |
| 5 | (visual) Failsafe stops motors when TX is killed mid-run |

Gates 1–3 are auto-verified in firmware. Gates 4–5 are visual checks during the LIVE phase that follows the range test.

---

## Safety design

This is the most dangerous test in the bringup sequence — motors actually spin. The sketch enforces several safeties:

1. **Arm-on-neutral.** Motors stay at 1500 µs (stop) until the operator centers the throttle stick *after* signal is acquired. If the stick happens to be at full throttle when the ESP32 boots, motors will NOT spin — the sketch refuses to arm and prints `[ARM BLOCKED]`.
2. **Forward cap.** Output is clamped to `MAX_FWD_US` (default **1800 µs ≈ 60% forward**) for the whole test. The stick can go to 2000 µs, but the ESC will never see more than 1800.
3. **No reverse by default.** Anything below 1500 µs is clamped to 1500 unless `ALLOW_REVERSE = true`. Bench tests don't need reverse; the props would walk the boat off the bench.
4. **Channel-freeze failsafe.** Flysky receivers keep streaming frames after signal loss with frozen values. Detected within 500 ms → motors forced to 1500, sketch disarms back to the neutral check, operator must re-center to re-arm.
5. **No-frame failsafe.** Receiver fully disconnected → same response within 500 ms.
6. **Re-arm on failsafe recovery.** Even after signal returns, motors stay at 1500 until the operator brings throttle to neutral. Prevents "stick was at 80%, signal blipped, motors slammed back to 80%" surprise.

---

## Hardware required

- ESP32 with iBUS divider on GPIO 16 (test_03 wiring)
- PCA9685 with logic at 3.3V; **ESC PWM signals come from PCA9685 ch0 / ch1**
- Both ESCs wired to motors, on **battery power** (separate from ESP32 USB power; common ground)
- Flysky receiver bound and powered
- **PROPS REMOVED** for first run, or boat clamped/strapped to the bench so it can't move

> ESC BECs are typically 5–6V. Do NOT feed BEC into the ESP32 — keep ESP32 on USB during bench testing. Tie all grounds together.

---

## Wiring (delta from test_07)

| From | To |
|------|----|
| Port ESC signal (orange/yellow) | PCA9685 ch0 signal |
| Starboard ESC signal | PCA9685 ch1 signal |
| ESC battery + / − | Battery (14.8 V LiPo or whatever the ESC expects) |
| ESC ground | common ground with PCA9685 GND and ESP32 GND |
| Motor leads | ESC motor outputs |

Receiver, voltage divider, and I2C wiring are unchanged from test_07.

---

## Tunables in the sketch

Top of `test_08_ibus_esc.ino`:

| Constant | Default | Purpose |
|----------|---------|---------|
| `THROTTLE_CHANNEL_INDEX` | `2` | iBUS index. `2` = CH3 = left-stick vertical (universal Mode-2 throttle). |
| `PORT_ESC_PCA_CHANNEL` | `0` | PCA9685 output for port ESC. |
| `STBD_ESC_PCA_CHANNEL` | `1` | PCA9685 output for starboard ESC. |
| `ALLOW_REVERSE` | `false` | Set `true` to allow stick below 1500 to drive reverse. |
| `MAX_FWD_US` | `1800` | Forward output cap. Raise carefully once you trust the test rig. |
| `MIN_REV_US` | `1200` | Reverse cap (only used if `ALLOW_REVERSE=true`). |
| `NEUTRAL_DEADBAND_US` | `30` | ±this around 1500 counts as "centered" for arming. |
| `ADVANCE_THRESHOLD_US` | `1700` | Throttle must reach this for PASS 2/3. |

---

## Flash and run

1. **Props off** (or boat secured).
2. Open `test_08_ibus_esc/test_08_ibus_esc.ino`. Board: ESP32 Dev Module.
3. Upload.
4. Open Serial Monitor at **115200**.
5. Power up ESCs (battery on). They will beep their arming chirp during the 3 s neutral-hold.
6. Turn on transmitter. Throttle stick **at the bottom or centered** (not at full).
7. Follow on-screen steps.

---

## Expected output (happy path)

```
========================================
  test_08_ibus_esc
========================================
SAFETY: PROPS OFF or boat firmly secured.
Throttle stick: CH3 (iBUS index 2)
ESCs: PCA9685 ch0 (port) + ch1 (starboard)
Limits: forward cap 1800 µs, reverse disabled

[INFO] PCA9685 found — arming ESCs at 1500 µs (3 s delay)...
[INFO] ESCs armed at hardware level. Motors will NOT spin until
       PASS 1/3 — throttle stick confirmed at neutral.

Step 1: turn on TX, center throttle stick (CH3 = left-stick vertical).
----------------------------------------

>>> SIGNAL ACQUIRED <<<
  Throttle channel reads 1500 µs.
----------------------------------------

PASS (1/3): throttle at neutral. ESCs ARMED — motors are LIVE.
Step 2: ADVANCE throttle stick forward (motors should spin).
----------------------------------------

PASS (2/3): throttle advanced (CH3=1735 µs, output capped at 1800 µs).
Step 3: return throttle to neutral.
----------------------------------------

PASS (3/3): throttle returned to neutral (1502 µs). Motors stopped.

========================================
  RANGE TEST PASSED
========================================
```

After the banner the sketch is in LIVE mode — throttle drives both motors continuously, capped at `MAX_FWD_US`. Test failsafe by killing the transmitter; you should see `>>> FAILSAFE <<<` and motors stop within ~500 ms.

---

## Failure modes the sketch surfaces

| Symptom in serial | What it means |
|-------------------|---------------|
| `[ARM BLOCKED] throttle is not at neutral` | Stick was up at boot. Center it; arming will continue. |
| `[WARN] checksum mismatch` (rate-limited) | iBUS wiring noise. Check divider resistors / wire length. |
| `[WARN] throttle channel out of range` | TX endpoints are misconfigured. Re-check TX channel limits. |
| `>>> FAILSAFE — channels frozen (TX off?)` | Flysky failsafe path. Motors forced to 1500. |
| `>>> FAILSAFE — no iBUS frames (RX disconnected?)` | Receiver power / wire fell off. |
| `[FAIL] PCA9685 not found` | I2C broken — re-run test_06. |
| `>>> FAILSAFE RECOVERED — re-center throttle to re-arm. <<<` | Signal back; throttle must return to neutral before motors spin again. |

---

## Gotcha — Radiolink CL9030 power switch is reverse-logic

The CL9030 ships with an inline power switch on the battery-positive lead. **Switch OPEN = ESC ON. Switch CLOSED = ESC OFF.** Counter-intuitive.

If the ESC is silent on power-up (no beep, no LED), the switch (or whatever you replaced it with) is closed, not open. Either leave the switch in place and use it normally, or **snip it out completely with no splice** — an open circuit through that lead is what powers the ESC. Splicing the wire closed kills it.

This was the failure mode on first test attempt 2026-04-25. Burned ~30 min before realizing.

---

## Pass criteria

- [x] `PASS (1/3)` — signal acquired, throttle at neutral, ESCs armed
- [x] `PASS (2/3)` — throttle advance recognized
- [x] `PASS (3/3)` — throttle returned to neutral
- [x] Both motors spin under throttle (visual)
- [x] Both motors stop cleanly at neutral (visual)
- [ ] Failsafe stops motors within ~500 ms of TX off — **deferred to in-hull test** (verified during pre-failure debugging that FAILSAFE / RECOVERED / RE-ARMED transitions print correctly; once motors were spinning, full TX-off test under load not retried in this session)

## Result — 2026-04-25

Both port and starboard motors spinning as expected. State machine and serial output behaved as designed throughout debugging (FAILSAFE, RECOVERED, RE-ARMED transitions all observed in the log).

**Status: PASS.**
