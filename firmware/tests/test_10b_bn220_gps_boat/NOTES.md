# test_10b_bn220_gps_boat — BN-220 in the Hull

Same test as `test_10_bn220_gps`, re-pinned for the **boat** (in-hull, permanent) wiring per the project pinout.

See `firmware/tests/test_10_bn220_gps/NOTES.md` for full bringup notes — pass criteria, library install, expected output, indoor TTFF caveat. This file only covers the deltas.

---

## Pin difference vs the bench test

| Wire | Bench (`test_10`) | Boat (`test_10b`) |
|------|-------------------|-------------------|
| GPS TX (green) → ESP32 RX | GPIO 18 | **GPIO 4** |
| GPS RX (white) ← ESP32 TX | GPIO 17 | GPIO 17 |

GPIO 4 is the permanent GPS RX pin per the project pinout. The bench test used GPIO 18 because that's what was on the breadboard.

---

## Purpose of this run

After installing the GPS in the hull and rewiring against other peripherals (ESCs, rudder servo, IMU, INA219, PCA9685), re-confirm:

1. The GPS still gets power at its own VCC pad.
2. NMEA bytes still arrive on GPIO 4.
3. A fix is still achievable through the hull (with the antenna pointed at sky, hatch open, or held near a window).
4. None of the surrounding rewiring broke anything — the test prints clear PASS/FAIL gates so a regression shows up immediately.

---

## Diagnostic order if PASS 1/3 doesn't appear

The BN-220's LED blinks at 1 Hz **whenever the chip is powered and outputting NMEA**, regardless of fix status. So:

- **LED dark** → almost always a power problem at the module. Multimeter 3.3V at the BN-220's own VCC pad. Don't trust upstream readings — a Wago or splice can flake on small wires and hide the break.
- **LED blinking but PASS 1/3 stuck** → power is fine, but the data line isn't reaching GPIO 4. Check the green wire continuity end-to-end and verify nothing else on the boat is driving GPIO 4.
- **PASS 1/3 but stuck on PASS 2/3** → bytes arrive but at the wrong baud. BN-220 default is 9600; if a previous sketch reconfigured the module, try 4800 or 38400.
- **PASS 2/3 but no fix for >5 min outdoors** → antenna obstruction, or the antenna got disconnected from the module during install.

---

## Pass criteria

- [ ] `PASS (1/3)` — bytes arriving on GPIO 4
- [ ] `PASS (2/3)` — valid NMEA parsed
- [ ] `PASS (3/3)` — position fix acquired through the hull
- [ ] (visual) lat/lon update sensibly when the boat is moved

## Status

Pending in-hull test.
