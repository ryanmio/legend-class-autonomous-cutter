# test_10b_bn220_gps_boat — BN-220 in the Hull

Same test as `test_10_bn220_gps`, re-pinned for the **boat** (in-hull, permanent) wiring per the project pinout.

See `firmware/tests/test_10_bn220_gps/NOTES.md` for full bringup notes — pass criteria, library install, expected output, indoor TTFF caveat. This file only covers the deltas.

---

## Pin difference vs the bench test

| Wire | Bench (`test_10`) | Boat (`test_10b`) |
|------|-------------------|-------------------|
| GPS TX (**white**) → ESP32 RX | GPIO 18 | **GPIO 17** |
| GPS RX (**green**) ← ESP32 TX | GPIO 17 | **GPIO 4**  |

Wire colors confirmed empirically 2026-05-03: this batch of BN-220s has **white=TX, green=RX** — REVERSED from the typical convention and from every doc/silkscreen we found. The boat harness physically has white going to GPIO 17 and green to GPIO 4, so `GPS_RX_PIN = 17` (where the ESP32 listens to the white wire).

---

## Purpose of this run

After installing the GPS in the hull and rewiring against other peripherals (ESCs, rudder servo, IMU, INA219, PCA9685), re-confirm:

1. The GPS still gets power at its own VCC pad.
2. NMEA bytes still arrive on GPIO 4.
3. A fix is still achievable through the hull (with the antenna pointed at sky, hatch open, or held near a window).
4. None of the surrounding rewiring broke anything — the test prints clear PASS/FAIL gates so a regression shows up immediately.

---

## Diagnostic order if PASS 1/3 doesn't appear

The BN-220 has two LEDs: **blue blinks at 1 Hz whenever NMEA is being output** (regardless of fix), **red is the PPS / fix indicator**. So:

- **Blue LED dark, getting `chars=1` then silence** → wire color reversal. This batch of BN-220s has white=TX, green=RX. If you wired `green→ESP32-RX` per the typical convention, the ESP32 is listening to the silent input pin while the module shouts into a deaf one. The single byte you see is noise on the floating line at boot, not real data. Swap the two wires and re-run.
- **Blue LED dark even after wire swap** → almost always a power problem at the module. Multimeter 3.3V at the BN-220's own VCC pad. Don't trust upstream readings — a Wago or splice can flake on small wires and hide the break.
- **Blue LED blinking but PASS 1/3 stuck** → power is fine and the module is talking, but the data line isn't reaching GPIO 17. Check the white wire continuity end-to-end and verify nothing else on the boat is driving GPIO 17.
- **PASS 1/3 but stuck on PASS 2/3** → bytes arrive but at the wrong baud. BN-220 default is 9600; if a previous sketch reconfigured the module, try 4800 or 38400.
- **PASS 2/3 but no fix for >5 min outdoors** → antenna obstruction, or the antenna got disconnected from the module during install.

---

## Pass criteria

- [x] `PASS (1/3)` — bytes arriving on GPIO 17 (white wire)
- [x] `PASS (2/3)` — valid NMEA parsed
- [x] `PASS (3/3)` — position fix acquired through the hull
- [x] (visual) lat/lon update sensibly when the boat is moved

## Result — 2026-05-03

5 satellites, TTFF = 28 s from boot. Fix: lat=38.864662, lon=-77.184843 (~108 m altitude), HDOP=1.43–1.92.

Root cause of the prior "stuck on chars=1" failures: **wire colors reversed from convention.** The boat harness was already wired green→GPIO 4 / white→GPIO 17, which is correct for this batch (white=TX, green=RX) — but the firmware had `GPS_RX_PIN = 4` (assuming green=TX), so the ESP32 was listening to the wrong pin. Swapping `GPS_RX_PIN` and `GPS_TX_PIN` in the sketch (and in `legend_cutter/config.h`) fixed it without touching a single wire in the boat.

**Status: PASS**
