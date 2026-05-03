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

## Phase 2 — drive the deck gun pan servo (TODO)

After phase 1 yields a channel number:

1. Edit `test_18_deck_gun_pan.ino`:
   - Comment out the discovery block in `handleFrame()`.
   - Add: read knob channel µs → clamp to safe pan range → write to PCA9685 ch8.
   - Add: PCA9685 init in `setup()` (mirror test_07 / test_15).
   - Add: no-frame failsafe — hold pan at `PWM_NEUTRAL` (1500 µs) when iBUS drops.
2. Add a sweep-limit calibration step similar to test_15 — find the safe mechanical extremes of the deck gun pan and clamp inside them, so the servo can't grind against the turret stops.

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

### Phase 2 (once written)
- [ ] Phase 1 channel cleanly drives ch8 servo with no jitter at rest
- [ ] Servo holds at 1500 µs on iBUS loss
- [ ] Sweep limits clamp the servo before it binds against turret stops

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

### Phase 2 — pending

Default clamp is `PAN_MIN_US..PAN_MAX_US = 1200..1800` µs. After running the
test, watch the running min/max line and adjust those constants down to the
*actual* mechanical safe extremes of the turret linkage. Same flow as test_15
for the rudder.

When phase 2 passes:
- update `firmware/legend_cutter/config.h:63` from `CH_GUN_PAN 3` → `8`
- add `IBUS_CH_GUN_PAN` define alongside the other iBUS channel constants
  (idx 4)
- record final clamp limits here in this section
