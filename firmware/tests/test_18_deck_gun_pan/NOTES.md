# test_18_deck_gun_pan — Knob → Forward Deck Gun Pan

Two-phase test. The same `.ino` file gets edited between phases — phase 1's discovery code is replaced/commented when phase 2 takes over.

---

## Phase 1 — channel discovery (current state of the sketch)

Goal: figure out which iBUS channel the knob on the FS-i6X is mapped to. The receiver streams 10 channels; we don't know off the top of our head which one the dial outputs on.

### How it works

- Sketch reads iBUS frames and compares each channel to its previous value frame-to-frame.
- If any channel moves by more than `MOVE_THRESHOLD_US` (default 20 µs), the sketch prints which one is moving.
- Per-channel output is rate-limited to one line per 150 ms so a continuous twist of the knob doesn't spam serial.
- Updates are only printed when the threshold is crossed, so a still-but-noisy channel stays quiet.

No baseline. No PCA9685. Pure iBUS sniffing.

### Hardware

- ESP32 with iBUS divider on GPIO 16 (1 kΩ + 2 kΩ to GND, same as test_03).
- Flysky FS-iA10B receiver bound and powered.
- That's it — no servo or PCA9685 needed in phase 1.

### Procedure

1. Open the sketch, upload (Board: ESP32 Dev Module, Serial Monitor 115200).
2. Power up the receiver. Turn on the TX. **Hands off the sticks.**
3. Wait for `[OK] iBUS frames arriving` and the initial-values dump.
4. Slowly turn ONLY the knob you want to use for deck gun pan.
5. Read the channel number from the `CHn is changing` lines.
6. Note both the channel number AND the µs range you can sweep through (e.g. `CH6: 1000..2000`). Phase 2 will need both.

### Expected output

```
========================================
  test_18_deck_gun_pan — phase 1: channel discovery
========================================
...
[OK] iBUS frames arriving. Watching for channel changes.
     Initial values: CH1=1500 CH2=1500 CH3=1500 CH4=1500 CH5=1500 CH6=1500 CH7=1500 CH8=1500 CH9=1500 CH10=1500
Now turn ONLY the knob you want to assign. Lines below
identify the channel that's moving.
----------------------------------------
CH6 (idx 5) is changing:  1500 → 1572  (Δ +72)
CH6 (idx 5) is changing:  1572 → 1641  (Δ +69)
CH6 (idx 5) is changing:  1641 → 1745  (Δ +104)
```

The CHn that fires is the one to use in phase 2. Sweep the knob fully each direction so you also capture its range.

### What if multiple channels light up

You bumped a stick. Stop, hold sticks still, turn only the knob. If a channel keeps moving with the dial completely still, that's transmitter noise or trim drift — not a real assignment.

---

## Phase 2 — drive the deck gun pan servo

> **The pan servo is now a POSITIONAL 9g micro** (MG90S / SG90 class), swapped
> in 2026-05-04 to replace the original FS90R continuous-rotation unit. µs
> commands an angle, not a speed. 1500 = centered, 1300/1700 = ±~30° from
> center (servo-dependent). Knob position straightforwardly maps to gun angle.

### Behavior

- Pass-through with clamp: knob µs (CH5) → servo µs (PCA9685 ch8), constrained to `[PAN_MIN_US..PAN_MAX_US]`.
- Center deadband (`PAN_DEADBAND_US ±15`) snaps output to exactly 1500 when the knob is near center — filters TX-side jitter so the servo doesn't micro-twitch at rest.
- No-frame and channel-freeze failsafes hold pan at 1500 µs on iBUS loss.
- Running min/max of commanded pan µs printed every 2 s — sweep the knob fully and you'll see how much travel the linkage actually allows before binding (test_15 pattern).

### History — earlier failed approaches (kept here so we don't repeat)

The original servo was an FS90R (continuous rotation, µs = speed). Two
attempts at making that work failed:

1. **Naïve passthrough** — knob µs → servo µs. Failed because the knob doesn't
   auto-center, so on TX power-on the gun started spinning immediately and
   wouldn't stop until the operator manually centered the knob.
2. **Toggle-switch arm + huge deadband** — would have gated the motor with a
   physical TX switch and used only the knob extremes for deliberate slewing.
   Phase 1 follow-up showed no switches are mapped on this transmitter and
   we'd need to dig into the FS-i6X menu to map one.

Hardware swap to positional servo solved the architectural problem cleanly.

### Hardware delta in phase 2

- PCA9685 powered (logic 3.3 V, V+ rail at servo voltage).
- Deck gun pan servo (MG90S, below-deck) signal → **PCA9685 ch8**.
- I²C SDA/SCL on GPIO 21 / 22.

### Note on `config.h` mismatch

`firmware/legend_cutter/config.h:63` currently defines:

```c
#define CH_GUN_PAN  3
```

That's a pre-hardware scaffold value. Real wiring puts the deck gun pan on **PCA9685 ch8**. Once phase 2 passes, update `CH_GUN_PAN` in `config.h` to `8`.

---

## Pass criteria

### Phase 1
- [ ] iBUS frames arrive within ~5 s of TX power-up
- [ ] Knob movement triggers `CHn is changing` lines
- [ ] Channel identified and µs range recorded below

### Phase 2 — PASS 2026-05-04
- [x] CH5 cleanly drives ch8 positional servo with no jitter at rest
- [x] Servo tracks knob across full ±500 µs range without binding
- [x] Direction corrected via `PAN_REVERSE = true`
- [x] Servo holds 1500 µs on iBUS loss (channel-freeze + no-frame failsafes)

---

## Result

### Phase 1 — 2026-05-03 PASS

Channel detected: **CH5 (iBUS index 4)**.
Range observed during sweep: at least 1363..1588 µs (only partial sweep recorded;
likely fuller range available — verify in phase 2 with the running min/max display).

Sample output during discovery:
```
CH5 (idx 4) is changing:  1588 → 1566  (Δ -22)
CH5 (idx 4) is changing:  1484 → 1461  (Δ -23)
CH5 (idx 4) is changing:  1388 → 1363  (Δ -25)
```

The phase 1 discovery sketch is preserved at the bottom of the `.ino` inside an
`#if 0` block — re-enable if you ever need to re-map a knob.

### Phase 2 — 2026-05-04 PASS (positional servo)

Final tunables:

```c
const uint16_t PAN_MIN_US      = 1000;
const uint16_t PAN_MAX_US      = 2000;
const uint16_t PAN_DEADBAND_US = 15;
const bool     PAN_REVERSE     = true;
```

Full ±500 µs range exposed — the operator confirmed the linkage sweeps
cleanly across the entire knob travel without binding, so no tighter
software clamp is needed. Direction flipped via `PAN_REVERSE = true`
because the knob's natural CW direction was driving the gun CCW.

### Followups (not blocking PASS)

- Update `firmware/legend_cutter/config.h:63` from `CH_GUN_PAN 3` → `8`
  to match real wiring.
- Add an `IBUS_CH_GUN_PAN` define alongside the other iBUS channel
  constants in `config.h` (idx 4 = CH5).
- Fix the `config.h:62` comment — it was right that the pan servo is a
  positional 9g micro, but only after the 2026-05-04 swap. The
  scaffold accidentally lined up with reality after the FS90R came
  out and the MG90S/SG90 went in.

When phase 2 passes:
- update `firmware/legend_cutter/config.h:63` from `CH_GUN_PAN 3` → `8`
- add `IBUS_CH_GUN_PAN` define alongside the other iBUS channel constants (idx 4)
- record final speed-cap and deadband values here
