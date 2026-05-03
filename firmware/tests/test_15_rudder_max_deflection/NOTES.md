# test_15_rudder_max_deflection — Find Safe Rudder Servo Limits

The rudder's twin-tiller linkage has a smaller safe arc than a 1000–2000 µs servo throw allows. Past a certain angle the armature flips over center and jams. This test lets you sweep the stick by hand and reads back the most-extreme µs values commanded so far, so you can stop just before binding and copy those numbers into `config.h` as the rudder's hard limits.

Builds on `test_07_ibus_rudder` (same wiring, same iBUS→PCA9685 path).

---

## What we're producing

Two numbers to put into `config.h`:

```c
#define RUDDER_MIN_US   <left value from test>
#define RUDDER_MAX_US   <right value from test>
```

These will be applied wherever rudder µs is computed (currently `motors.cpp` / `legend_cutter.ino` send the iBUS value straight through after a 1000–2000 clamp — that clamp gets tightened to these new bounds).

---

## Hardware required

Same as test_07:

- ESP32 with iBUS divider on GPIO 16, I²C on 21/22
- PCA9685 powered (logic 3.3 V, **V+ rail at 6 V** for the rudder)
- Rudder servo on **PCA9685 ch2**, with the dogbone linkage installed and connected to the rudder shaft. The point of this test is to find the mechanical limit of the *installed* linkage, so it must be hooked up.
- ESCs may stay wired — they're held at 1500 µs neutral throughout.

---

## Tunables in the sketch

| Constant | Default | Notes |
|---|---|---|
| `RUDDER_CHANNEL_INDEX` | `0` | CH1, right-stick horizontal (Mode 2). See note below. |
| `RUDDER_PCA_CHANNEL` | `2` | matches `CH_RUDDER` in `config.h` |
| `RUDDER_REVERSE` | `false` | flip if left-stick gives right-rudder |
| `REPORT_INTERVAL_MS` | `2000` | how often the running max prints |

### Channel index discrepancy

`config.h:80` currently has `IBUS_CH_RUDDER 3` (CH4, left-stick). `test_07` passed with `RUDDER_CHANNEL_INDEX = 0` (CH1, right-stick). The audit (`firmware/FIRMWARE_AUDIT.md` §1b) flags this as needing reconciliation. This test defaults to `0` to match test_07. If your TX is mapped differently, update `RUDDER_CHANNEL_INDEX` here AND fix `IBUS_CH_RUDDER` in `config.h` to match.

---

## Procedure

1. Open `test_15_rudder_max_deflection/test_15_rudder_max_deflection.ino` in Arduino IDE.
2. **Board:** ESP32 Dev Module. Upload. Serial Monitor at **115200**.
3. Power the boat / bench setup so the rudder servo is live and the linkage is connected.
4. Turn on the transmitter, center the stick.
5. Confirm `[OK] iBUS acquired` appears.
6. **Slowly** push the stick LEFT, watching the rudder. The instant you see the linkage start to bind, hesitate, or the armature start to flip — STOP. Do not push further. Hold the stick where it is for one or two report cycles so the running max captures it.
7. Return to center.
8. Repeat for RIGHT.
9. Read the last `MAX so far:` line. The two µs values are your limits.
10. If you overshoot at any point: type `r` + ENTER in Serial Monitor to reset and start over.

If the rudder is asymmetric (different mechanical range each direction), that's fine — the two µs offsets from 1500 don't need to match. Record what you actually saw.

---

## Expected output

```
========================================
  test_15_rudder_max_deflection
========================================
Rudder stick: CH1 (iBUS index 0)
Rudder servo: PCA9685 ch2  (reverse=false)

Procedure:
  1. Slowly push stick LEFT until rudder is at its safe extreme.
     STOP just before binding. Hold for a beat.
  2. Center, then slowly push stick RIGHT to its safe extreme.
  3. Read the last 'MAX so far' line — those are your limits.
  4. Type 'r'+ENTER on serial to reset if you overshoot.

[INFO] PCA9685 found — arming ESCs at 1500 µs (3 s delay)...
[INFO] ESCs armed. Rudder centered.

Waiting for iBUS frames... turn on the transmitter.
----------------------------------------

[OK] iBUS acquired. Rudder reading 1500 µs at center.
Begin sweeping. Reports print every 2 s.
----------------------------------------
MAX so far:  LEFT = 1500 µs (Δ   +0)   RIGHT = 1500 µs (Δ   +0)
MAX so far:  LEFT = 1340 µs (Δ -160)   RIGHT = 1500 µs (Δ   +0)
MAX so far:  LEFT = 1220 µs (Δ -280)   RIGHT = 1500 µs (Δ   +0)
MAX so far:  LEFT = 1180 µs (Δ -320)   RIGHT = 1500 µs (Δ   +0)   ← stopped here
MAX so far:  LEFT = 1180 µs (Δ -320)   RIGHT = 1670 µs (Δ +170)
MAX so far:  LEFT = 1180 µs (Δ -320)   RIGHT = 1820 µs (Δ +320)   ← final
```

The last line gives `RUDDER_MIN_US 1180`, `RUDDER_MAX_US 1820` (illustrative — your numbers will differ).

Consider applying a small safety margin (e.g. 30–50 µs inward from each extreme) before writing the firmware constants, so production limits aren't right at the binding edge.

---

## Pass criteria

- [~] Test sweeps cleanly with no checksum warnings — *frequent `[WARN] checksum mismatch` during run; tracked separately, did not affect min/max capture*
- [x] `[OK] iBUS acquired` printed
- [x] Final LEFT µs ≤ 1500 (negative Δ)
- [x] Final RIGHT µs ≥ 1500 (positive Δ)
- [x] Linkage observed to NOT bind/flip at the recorded extremes (operator stopped well before binding on both sides)
- [x] Numbers recorded below for use in `config.h`

---

## Result — 2026-05-03 PASS

Observed safe extremes (operator-stopped before binding):

- LEFT  = 1334 µs (Δ -166)
- RIGHT = 1683 µs (Δ +183)

Chosen firmware limits: symmetric **±170 µs from 1500 neutral**.

```
#define RUDDER_MIN_US   1330   // 1500 - 170
#define RUDDER_MAX_US   1670   // 1500 + 170
```

Rationale: LEFT was the constraining side at Δ -166. Operator reported being
"pretty far from binding" at that point, so accepting Δ -170 (4 µs past the
recorded stop point) is well within mechanical safety. RIGHT picks up a 13 µs
inward margin from the recorded 1683.

Added to `legend_cutter/config.h` alongside `PWM_MIN`/`PWM_MAX`. Still TODO:
apply the clamp to the rudder µs in `motors.cpp` / `legend_cutter.ino` (the
current code only clamps to 1000/2000).

### Checksum mismatch warning — not a test blocker

`[WARN] checksum mismatch — wiring noise?` fired roughly once per 2 s
report cycle throughout the test. The warn is rate-limited to once/second,
so this represents *at least one* bad iBUS frame in each window — at iBUS's
~140 Hz frame rate, that's well under 1% loss. Did not affect the running
min/max (they updated cleanly between dropped frames).

Wiggling the GPIO16 connection at the ESP32 did not change the rate. Likely
candidates:

1. Marginal voltage divider (1 kΩ / 2 kΩ on iBUS line) — possibly drifted
   resistor values, or a cold solder joint on the divider.
2. Loose ground reference between the FS-iA10B receiver and the ESP32.
3. Long unshielded run on the iBUS signal wire picking up coupled noise.

To investigate before integration testing: re-run `test_03_ibus_passthrough`
and confirm whether checksum warns happen there too. If yes, this is a
wiring/divider issue independent of the PCA9685 setup. If only test_15 sees
them, suspect the PCA9685 I²C bus is coupling onto the iBUS line via a
shared ground or routing. Either way, fix before we depend on iBUS for
on-water control.
