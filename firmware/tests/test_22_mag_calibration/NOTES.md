# test_22_mag_calibration — ICM-20948 Magnetometer Hard-Iron Calibration

Derives hard-iron offsets for the AK09916 magnetometer inside the ICM-20948.
Spins the boat through a full 360° horizontal rotation; exits automatically
when ranges plateau (confirming a complete circle), not on a timer.
After calibration, send `H` in Serial Monitor for a single tilt-compensated
heading snapshot.

---

## Pass criteria

| Gate | What |
|------|------|
| GATE 1 | WiFi connects, IP printed |
| GATE 2 | 30-second spin window completes without error |
| GATE 3 | Plateau detected — rangeX and rangeY both > 20 µT |
| GATE 4 | `H` snapshot matches known compass bearing ±15° |
| GATE 5 | Two `H` snapshots with boat held still differ < 5° |

---

## Calibration run history

### Run 1 — 2026-05-08 (INVALID — incomplete spin)

Sketch used a blind 30-second timer (no plateau detection).
User did not complete a full 360° rotation. Gate 3 "passed" only because the
range happened to exceed 20 µT, not because the spin was verified complete.
**Low confidence. Do not use these offsets.**

```
#define MAG_OFFSET_X  -21.98f
#define MAG_OFFSET_Y   4.20f
#define MAG_OFFSET_Z  -20.55f
```

### Run 2 — 2026-05-08 (VALID — plateau confirmed)

Sketch updated with plateau detection. Exits only when ranges stop growing,
confirming a complete rotation was captured.

```
Samples: 1310
Axis  min      max      range    offset
X    -76.80    -0.45   76.35   -38.62
Y    -23.85    27.15   51.00     1.65
Z    -34.20     4.50   38.70   -14.85
```

```c
#define MAG_OFFSET_X  -38.62f
#define MAG_OFFSET_Y   1.65f
#define MAG_OFFSET_Z  -14.85f
```

**[GATE 3] PASS — plateau detected, full rotation confirmed.**
Gates 4 and 5 pending (heading verification vs. known compass bearing).

#### Comparison between runs

The X axis offset shifted by ~17 µT between runs (-21.98 → -38.62), which is
expected: an incomplete spin undersamples the X-axis extremes, pulling the
midpoint toward zero. The large X range (76.35 µT) reflects significant
hard-iron bias along that axis — consistent with a boat that has servos,
ESCs, and wiring running parallel to the bow-stern axis.

Y and Z offsets changed by smaller amounts (Y: +4.20 → +1.65, Z: -20.55 →
-14.85). Run 2 values are the ones to use.

---

## Where these offsets go next

Pass the `#define` lines into **test_23** as compile-time constants.
In the production firmware (`imu.cpp`), `imuCalibrateMag()` stores offsets
in NVS under namespace `imu_cal` — the test_22 values can seed that NVS
write before first on-water use, or test_23 can be used to confirm heading
quality before committing them to NVS.
