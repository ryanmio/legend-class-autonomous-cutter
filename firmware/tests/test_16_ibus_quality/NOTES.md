# test_16_ibus_quality — iBUS Link Quality Meter

A measurement tool, not a pass/fail bringup test. Run it before and after each mitigation (twist wires, add filter cap, reroute, fix grounding) and compare the loss percentages directly.

Triggered by test_15's persistent ~1 checksum-warn-per-second observation. test_03 just *prints* warns; this test *counts* them and produces a number you can compare.

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

Same as test_03:

- ESP32, iBUS divider on GPIO 16 (1 kΩ to GPIO, 2 kΩ to GND)
- FS-iA10B receiver bound and powered, GND tied to ESP32 GND
- Transmitter on

PCA9685 / servos / ESCs **disconnected** (or unpowered) — we want to characterize the iBUS path in isolation. If you want to characterize *with* the I²C bus active, run after the iBUS-only baseline so you have a comparison.

---

## Procedure

1. Flash, open Serial @ 115200, transmitter on.
2. Wait for `[OK] iBUS acquired`.
3. Let it run **at least 30 s** before trusting the cumulative number — windows shorter than that are noisy.
4. Make a change (twist iBUS pair, add 100 nF cap GPIO16→GND at the ESP32, fix a ground, etc.).
5. Type `r` + ENTER to reset counters.
6. Let run another 30 s. Compare.

Record the before/after numbers in this NOTES file under "Runs" so we have a paper trail.

---

## Expected output

```
========================================
  test_16_ibus_quality
========================================
Counts iBUS frame quality over rolling windows.
No PCA9685 / servos. iBUS path only.
Window: 5000 ms.  Type 'r'+ENTER to reset counters.

Healthy: total loss <0.05%, resync_bytes flat after startup.
Marginal: 0.05–0.5%. Bad: >0.5% or resync_bytes growing.

Waiting for iBUS frames... turn on the transmitter.
----------------------------------------
[OK] iBUS acquired. Counting...
----------------------------------------
[t=  5s]  win:  698 good /   4 bad (0.57%) | total:   698/   4 (0.57%)
          resync_bytes=2  bad_header=0
[t= 10s]  win:  696 good /   3 bad (0.43%) | total:  1394/   7 (0.50%)
          resync_bytes=2  bad_header=0
[t= 30s]  win:  697 good /   2 bad (0.29%) | total:  4178/  21 (0.50%)
          resync_bytes=2  bad_header=0
```

A small one-time bump in `resync_bytes` at startup (a few bytes before the first valid header) is normal. Continued growth is not.

---

## Diagnostic decision tree

1. **Baseline run** (this test, no PCA, no ESCs): note loss %.
2. If **loss < 0.05 %** at baseline → iBUS path is fine. The warns under test_15 came from coupling once the PCA was added. Fix at integration (shorter iBUS run away from I²C, decoupling cap on PCA power, etc.).
3. If **loss ≥ 0.05 %** at baseline → fix at the iBUS path itself. In rough order of likelihood / cost:
   1. Multimeter continuity RX-GND ↔ ESP32-GND. Want ~0 Ω.
   2. Twist the iBUS signal wire with its ground return for the full length of the run.
   3. 100 nF ceramic cap from GPIO 16 to ESP32 GND, soldered as close to the ESP32 pin as possible.
   4. Re-solder / replace the divider resistors.
   5. Replace the iBUS wire with a shielded run (last resort given hull constraints).

After each step, reset counters (`r`+ENTER) and re-measure for ≥ 30 s. Stop when loss is < 0.05 %.

---

## Runs

```
YYYY-MM-DD  config                          loss%   resync  bad_hdr  notes
----------  ------                          -----   ------  -------  -----
2026-05-03  PCA still connected, no motors  1.12%     5576*       1  baseline. resync frozen
                                                                     after acquisition (startup
                                                                     hunt, not growing). Single-
                                                                     frame errors only, no bursts
                                                                     observed. Decision: continue
                                                                     build, retest under motor
                                                                     load at integration.
```

\* `resync_bytes` accrued during initial frame hunt before first valid header — frozen post-acquisition, so not a live failure mode.

### Pending

- [ ] Multimeter continuity RX-GND ↔ ESP32-GND (want ~0 Ω)
- [ ] Optional: 100 nF cap GPIO 16 → ESP32 GND if convenient
- [ ] **At integration**: re-run with ESCs at 30% throttle. If loss > 1% or bursts appear, address before on-water.
