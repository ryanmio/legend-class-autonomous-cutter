You are continuing development of the Legend Cutter autonomous sailing boat
(/Users/ryan/Documents/GitHub/legend-class-autonomous-cutter/). This is a React Native / Expo SDK 54 app
(TypeScript) paired with Arduino/ESP32 firmware in firmware/tests/. There is no production firmware yet —
only test sketches, built incrementally.

Approach this like a senior Navy systems engineer. Every test must be proven before the next layer is added.
No skipping gates. No water testing yet.

---
## What has been proven

- test_20/21: WiFi HTTP polling, LED control, GPS fix on map
- test_22: ICM-20948 hard-iron calibration. Offsets: MAG_OFFSET_X=-38.62, MAG_OFFSET_Y=1.65, MAG_OFFSET_Z=-14.85
- test_23: WiFi + GPS + IMU complementary filter. TRACKING PASS (−363.1° cumulative, 3.1° return error).
- test_24: PCA9685 + ICM-20948 on shared I2C bus confirmed. Proportional heading hold on bench confirmed working (rudder responds to yaw rotation).
- test_25: GPS waypoint → haversine bearing → heading hold. Partially working — see open bug below.

---
## Active sketch

`firmware/tests/test_25_waypoint_heading/test_25_waypoint_heading.ino`

Serial commands: W (waypoint snapshot), G (gate check), H<deg> (manual target), K<val> (set Kp), D (raw IMU diag), S (stability), T (tracking)

---
## Open bug: absolute IMU heading is wrong

The heading hold controller works mechanically — the rudder responds to error and drives it toward zero. But the absolute heading the IMU reports is wrong, so the rudder goes neutral when the boat is pointing in the wrong physical direction.

**What is confirmed:**
- Gyro tracking is correct (tracking test passes — rotation is measured accurately)
- Tilt no longer affects heading (was fixed: `mr_z = -mx` in the mag remapping)
- The hard-iron offsets from test_22 (measured indoors surrounded by metal desk, tools, ballast) corrupt the heading badly when used outdoors — they are currently **zeroed** and should stay zeroed until recalibrated outdoors with the boat fully built
- Even with zero offsets the heading is still wrong — approximately reversed

**What is NOT confirmed:**
- Which physical direction chip Y and chip Z point relative to the boat bow. This is the root of the problem. The previous agent made several formula changes based on assumptions about chip orientation without measuring it.

**IMU physical facts:**
- Chip X axis points UP (confirmed: ax ≈ +984 mg constant across all yaw rotations)
- my and mz are the two horizontal axes (they vary during yaw rotation, mx stays flat)
- The chip is mounted on a vertical superstructure wall

**Current remapping in the sketch:**
```cpp
float ar_x = ay,  ar_y = az,  ar_z = ax;   // accel
float mr_x = -my, mr_y = mz,  mr_z = -mx;  // mag
```
The `mr_z = -mx` (negating chip X for tilt compensation) is confirmed correct — do not revert it.
The `mr_x = -my` sign is unconfirmed and may be wrong.

---
## How to actually fix this

**Do not change any formula until you have measured the chip orientation.**

The `D` command prints raw mx, my, mz. Use it:

1. Hold the boat still pointing at a known compass bearing (use phone compass app). Type `D`. Record my and mz values.
2. Rotate 90°. Type `D` again. Record.
3. From these two readings you can determine exactly which axis is bow-aligned and what sign it has. Then derive the correct formula with certainty.

The heading formula for a tilt-compensated compass is standard — the only unknown is the axis-to-physical-direction mapping. Measure it, don't assume it.

---
## Critical rules — do not violate

- **Serial output**: tests print a single PASS/FAIL result block and nothing else. No streaming, no per-second updates, no live values during timed windows. The `D` command is the only exception (it's an explicit diagnostic snapshot). This rule has been violated repeatedly and Ryan gets very frustrated.
- **Commit discipline**: commit after writing each test sketch, commit again on PASS. Do not wait to be asked.
- **GPS wiring**: BN-220 white wire = TX (reversed from convention). ESP32 RX → white wire. GPS_RX_PIN=17, GPS_TX_PIN=4.
- **WiFi priority**: home WiFi → iPhone hotspot ("Ryan's iPhone") → AP fallback "LegendCutter". Never change this order.
- **No water testing** until bench validation is complete.
- Ryan is a Mode-2 RC pilot — right stick is throttle/rudder (CH1).

---
## Key files

- `firmware/tests/test_25_waypoint_heading/` — active sketch and NOTES
- `firmware/tests/test_22_mag_calibration/NOTES.md` — how offsets were measured
- `firmware/tests/test_23_wifi_gps_imu/NOTES.md` — axis remapping discovery
- `firmware/FIRMWARE_AUDIT.md` — full picture of what's proven vs stub
- `app/src/screens/MapScreen.tsx` — tap-to-set waypoint, bearing display
