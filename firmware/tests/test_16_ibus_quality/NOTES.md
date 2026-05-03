# test_16_ibus_quality — iBUS Link Quality Meter (Under Load)

A measurement tool, not a pass/fail bringup test. Run it before and after each mitigation (twist wires, add filter cap, reroute, fix grounding) and compare the loss percentages directly.

Drives both ESCs from the throttle stick (CH3) and the rudder from CH1 (clamped to `RUDDER_MIN_US`/`RUDDER_MAX_US`) while counting iBUS frame quality. Throttle is bucketed into 5 bands (0-20%, 20-40%, 40-60%, 60-80%, 80-100%) so you can see at a glance whether loss correlates with motor current.

Originally written quiescent-only after test_15 surfaced a persistent ~1 checksum-warn-per-second. Extended to run under load because the real question — does motor EMI degrade the link enough to matter — only gets answered with motors actually spinning.

**SAFETY:** props off, boat cradled, throttle stick at zero before powering on. ESCs will spin both motors throughout.

---

## What it counts

| Counter | Meaning |
|---|---|
| `good` | Frame parsed cleanly: header `0x20 0x40` + checksum valid |
| `bad_checksum` | Header valid but checksum wrong → bit flipped mid-frame |
| `bad_header` | 32 bytes consumed but header wrong → lost framing (worse) |
| `resync_bytes` | Bytes discarded while hunting for `0x20` header |

Loss % = `(bad_checksum + bad_header) / (good + bad_checksum + bad_header)`.

iBUS sends ~140 frames/sec, so a 5-second window gives ~700 frame samples — enough to nail the percentage to two decimal places.

---

## Quality bands (for this rig)

| Loss | Verdict |
|---|---|
| < 0.05 % | Healthy. Move on. |
| 0.05 – 0.5 % | Marginal. Will get worse under motor load — fix before integration. |
| > 0.5 % | Bad. Diagnose now. Anything in the same 1 s window as a multi-frame burst will failsafe. |
| `resync_bytes` growing | Stream losing framing — a worse failure mode than checksum loss. Almost always a wiring problem. |

---

## Hardware required

- ESP32, iBUS divider on GPIO 16 (1 kΩ to GPIO, 2 kΩ to GND)
- FS-iA10B receiver bound and powered, GND tied to ESP32 GND
- PCA9685 powered (logic 3.3 V, **V+ at 6 V** for the rudder)
- Both ESCs wired to PCA9685 ch0 (port) / ch1 (stbd), powered from main pack
- Rudder servo on PCA9685 ch2 with linkage installed
- **Props OFF, boat in a cradle.** Both motors will spin.

---

## Procedure

1. Confirm props are off and the boat is restrained.
2. Throttle stick to **zero** before applying power.
3. Flash, open Serial @ 115200, transmitter on.
4. Wait for `[INFO] ESCs armed.` then `[OK] iBUS acquired. Outputs live.`
5. Let it sit at idle for ~10 s — that's your idle-band baseline.
6. Slowly ramp throttle through each band (20%, 40%, 60%, 80%) and **hold ~15 s in each** so the band counters get ≥ 2000 samples each.
7. Watch the per-window line. The `bands(loss%):` summary updates live.
8. After the ramp, type `d` + ENTER to print the full histogram. Type `r` + ENTER to reset between configurations / mitigations.

Record results in the "Runs" log below.

---

## Expected output

```
========================================
  test_16_ibus_quality (under load)
========================================
Counts iBUS frames while driving ESCs + rudder live.
Window: 5000 ms.  Commands: 'r'=reset, 'd'=dump bands.

** SAFETY: PROPS OFF.  Boat secured.  Throttle at zero. **

[INFO] PCA9685 found. Arming ESCs at 1500 µs (3 s)...
[INFO] ESCs armed. Outputs live once iBUS acquires.

Waiting for iBUS frames... turn on the transmitter.
----------------------------------------
[OK] iBUS acquired. Outputs live. Counting...
----------------------------------------
[t=  5s] thr=  0%  win:  698 good /   4 bad (0.57%) | total:   698/   4 (0.57%)
         bands(loss%): 0-20:0.57 | 20-40: -- | 40-60: -- | 60-80: -- | 80+: --
[t= 20s] thr= 45%  win:  690 good /  10 bad (1.43%) | total:  2700/  21 (0.77%)
         bands(loss%): 0-20:0.43 | 20-40:0.85 | 40-60:1.43 | 60-80: -- | 80+: --
[t= 60s] thr= 90%  win:  650 good /  50 bad (7.14%) | total:  7800/ 350 (4.29%)
         bands(loss%): 0-20:0.43 | 20-40:0.85 | 40-60:1.43 | 60-80:5.20 | 80+:7.14
```

Numbers are illustrative. The shape that matters: do the band loss percentages stay flat, or do they climb with throttle?

- **Flat across bands** → motor EMI isn't the dominant noise source. Whatever's causing the ~1% baseline is something else (divider, grounding, I²C coupling), addressable later.
- **Rises with throttle** → ESC switching is coupling onto the iBUS line. Mitigations: shorter parallel run, twisted pair, ferrite bead on the iBUS wire, decoupling cap at ESP32.

A small one-time bump in `resync_bytes` at startup (a few bytes before first valid header) is normal. Continued growth during the run is not.

---

## Diagnostic decision tree

Look at the band histogram after a full throttle ramp.

1. **All bands < 1% AND flat (no monotonic climb)** → link is fine for on-water. Move on.
2. **Bands < 1% but climbing with throttle** → motor EMI is starting to show but headroom is OK. Note for re-measurement after on-water runs; defer mitigation.
3. **Any band ≥ 5%** → fix before on-water. Risk of failsafe-trip during a turn. Try in this order:
   1. Multimeter continuity RX-GND ↔ ESP32-GND. Want ~0 Ω.
   2. 100 nF ceramic cap from GPIO 16 to ESP32 GND, soldered as close to the ESP32 pin as possible.
   3. Twist the iBUS signal wire with a dedicated ground return for the full length of the run.
   4. Snap-on ferrite bead around the iBUS wire near the ESP32 (cheap; helps with high-frequency ESC switching noise specifically).
   5. Re-solder / replace the divider resistors.
4. **`bad_header` or `resync_bytes` growing during the run** → losing framing, not just bit errors. Same fixes as above but more urgent — start with #1.

After each fix, `r`+ENTER and re-ramp.

---

## Runs

### Quiescent baseline (pre-load-driving sketch)

```
YYYY-MM-DD  config                          loss%   resync  bad_hdr  notes
----------  ------                          -----   ------  -------  -----
2026-05-03  PCA connected, no motors        1.12%     5576*       1  baseline. resync frozen
                                                                     after acquisition (startup
                                                                     hunt, not growing). Single-
                                                                     frame errors only, no bursts.
```

\* `resync_bytes` accrued during initial frame hunt before first valid header — frozen post-acquisition, so not a live failure mode.

### Under-load runs

Record per-band loss % across the throttle ramp. Format below; fill the band columns with the values from the `d` (dump) command.

```
YYYY-MM-DD  config                  0-20    20-40   40-60   60-80   80+    notes
----------  ------                  ----    -----   -----   -----   ---    -----
                                    ?.??%   ?.??%   ?.??%   ?.??%   ?.??%
```
