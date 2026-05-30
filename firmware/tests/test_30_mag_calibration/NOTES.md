# test_30_mag_calibration — Notes

## Status: PENDING (sketch built 2026-05-30, bench-test not yet run)

Proves the plumbing that test_29 will inherit for app-driven mag
calibration:
- Plateau-detect cal procedure runs onboard when triggered via HTTP
  (not at boot — that was test_22's model).
- Offsets persist in ESP32 NVS namespace `imu_cal`, survive power
  cycles, applied to live heading immediately.
- Heading math matches test_29 exactly (axis remap + tilt
  compensation) so the resulting offsets port directly into test_29
  without recalibration.

Bench-only. No motors, GPS, audio, bilge. ICM-20948 only.

---

## Pass criteria

| Gate | What |
|------|------|
| GATE 1 | Boots, WiFi connects, IP printed. ICM-20948 init OK. |
| GATE 2 | Fresh-NVS boot: `/telemetry` returns `mag_cal_state="idle"`, `mag_calibrated=false`, offsets all 0. |
| GATE 3 | `POST /calibrate_mag/start` → `mag_cal_state` transitions to `"collecting"`, `mag_cal_progress` climbs as the boat is rotated. |
| GATE 4 | Within 60 s of rotation: `mag_cal_state="done"`, `mag_off_x/y/z` populated, `mag_baseline_uT > 20`. Serial prints offset summary. |
| GATE 5 | Power-cycle ESP32 → on reboot `/telemetry` shows `mag_calibrated=true` with the same `mag_off_x/y/z` and `mag_baseline_uT` from before the reboot. |
| GATE 6 | Heading reads accurate vs a known compass bearing (±15°). |
| GATE 7 | Two heading snapshots with the boat held still differ < 5°. |
| GATE 8 | `POST /calibrate_mag/abort` during a `collecting` run transitions back to `"idle"`; NVS unchanged from prior cal. |
| GATE 9 | `POST /calibrate_mag/start` while `mag_cal_state=="done"` restarts the procedure cleanly. |

GATE 6 + 7 are the real-cal verification gates that test_22 never
formally passed (its NOTES list them "pending"). Get them this time.

---

## How to bench-test

1. **Flash sketch.** Connect ICM-20948 (SDA=21, SCL=22, AD0=GND).
   Copy `secrets.h.example` → `secrets.h`, fill in WiFi.
2. **Open Serial Monitor** at 115200. Confirm WiFi IP printed.
3. **GATE 1 + 2:** `curl http://<ip>/telemetry | jq .` should show
   `mag_cal_state: "idle"` and either all-zero offsets (fresh NVS) or
   prior cal values (NVS already populated).
4. **GATE 3:** Trigger cal —
   `curl -X POST http://<ip>/calibrate_mag/start`.
   Slowly rotate the breakout board (or whole boat if assembled)
   through a full 360° horizontal circle, ideally 2-3 rotations over
   ~30 s. Watch `mag_cal_progress` climb in `/telemetry`.
5. **GATE 4:** When the procedure auto-finishes, `mag_cal_state`
   should read `"done"`. Note the offsets in `/telemetry`.
6. **GATE 5:** Power-cycle the ESP32 (unplug USB or RST). After
   reboot, `/telemetry` should show the same offsets — proves NVS
   persistence.
7. **GATE 6 + 7:** Place the board so it points at a known compass
   bearing (use a phone compass app). `heading` in `/telemetry`
   should match ±15°. Hold still, take two readings — should agree
   within 5°.
8. **GATE 8:** Re-run start, then before plateau,
   `curl -X POST http://<ip>/calibrate_mag/abort`. State should go
   back to `idle`, offsets unchanged.
9. **GATE 9:** With state=`done`, re-POST `/calibrate_mag/start`.
   New cal cycle should run cleanly and overwrite NVS on success.

---

## After PASS

1. Update this NOTES.md status header to PASS with the cal values.
2. Port the NVS + state machine + endpoints into
   `firmware/tests/test_29_pool_integration/` (the active boat
   firmware). Replace its hardcoded `MAG_OFFSET_*` defines with
   NVS-loaded variables.
3. Build the app `CalibrationScreen` + HelmScreen banner integration.
4. Lake test.

See `CALIBRATION_PLAN.md` (gitignored) for the full design.
