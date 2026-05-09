# test_23_wifi_gps_imu — Results

## Gates

| Gate | Description | Result |
|------|-------------|--------|
| GATE 1 | WiFi connects, Serial shows IP | PASS |
| GATE 2 | App SCAN finds boat, telemetry shows uptime incrementing | — not checked |
| GATE 3 | GPS fix acquired, position appears on map | — not checked |
| GATE 4 | Point bow at known bearing, type number, error < 15° | FAIL (see notes) |
| GATE 5 | Heading variance < 5° over 5 s held still | PASS — range 1.7° |
| GATE 6 | Helm screen shows heading number instead of `--` | — not checked |
| TRACKING | Cumulative 360° rotation closes within 20° | PASS — −363.1° cumulative, 3.1° return error |

## Notes

**GATE 4 — absolute accuracy not confirmed indoors.**
Two attempts: errors of 35° and 29°. iPhone compass reference was unreliable (±10° variance indoors). The heading is tracking correctly (TRACKING PASS is definitive proof) but the absolute offset between chip Y axis and boat bow is unknown. This offset will be measured on the water using GPS course-over-ground as the reference and hardcoded as `MOUNTING_OFFSET_DEG` before test_24.

**Axis remapping discovered during bringup.**
ICM-20948 mounted on vertical superstructure wall with X axis pointing up (confirmed by diagnostic: ax≈+984 mg constant across yaw rotations). Standard Z-up tilt compensation formulas required axis remapping: accel `[ay, az, ax]`, mag `[my, mz, mx]`. Gravity-projected yaw rate formula was already orientation-independent and required no change.

**GATES 2, 3, 6 — deferred.**
Not verified during this session. Check before closing test_23 formally:
- GATE 2: connect app, confirm uptime ticks on Telemetry screen
- GATE 3: get GPS fix outdoors, confirm position on Map screen
- GATE 6: confirm Helm screen heading field shows a number
