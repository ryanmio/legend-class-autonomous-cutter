# test_25_waypoint_heading — Results

## Gates

| Gate | Description | Result |
|------|-------------|--------|
| GATE 1 | GPS fix acquired, position valid | — |
| GATE 2 | Drop waypoint in app, firmware telemetry shows wp_bearing | — |
| GATE 3 | App-shown bearing and firmware wp_bearing agree within 1° | — |
| GATE 4 | Rotate boat until rudder nearly straight, type 'W': heading error < 20° | — |
| GATE 5 | Move waypoint on app map, wp_bearing updates in telemetry | — |

## Notes

*Test not yet run.*

### What this test proves

Full math pipeline: GPS lat/lon → haversine bearing → proportional heading hold.
App and firmware each compute the bearing independently (TypeScript vs C++).
Gate 3 confirms both answers match — proving the haversine math is correct in
both places before trusting either.

Gate 4 connects the math to the physical world: rotate the boat until the rudder
goes nearly straight, then snapshot. The error at that point is the difference
between where the IMU thinks the boat is pointing and where the haversine says
it should point — which is the mounting offset plus any IMU drift.

### Procedure

1. Flash. Go outdoors or near a window. Wait for GPS fix (Serial shows sats).
2. Open app → Map screen. You should see the boat marker at your location.
3. Drop a waypoint on the map (tap). The HUD shows the bearing: e.g. "→ WP: 090.1°".
4. Check Serial: `wp_bearing` in telemetry should match the app HUD within 1° → GATE 3.
5. Rotate the boat by hand until the rudder sits close to neutral.
6. Type 'W' in Serial Monitor → GATE 4 PASS if error < 20°.
7. Tap a different spot on the map. Confirm `wp_bearing` changes in telemetry → GATE 5.

### Mounting offset

`MOUNTING_OFFSET_DEG = 0.0f` (default). The 'W' snapshot will show the actual
error between the haversine bearing and the IMU heading when the rudder is
straight. That number is the mounting offset. Record it here and hardcode it
before the first water test.

Measured offset: **TBD** (to be determined on water via GPS course-over-ground)

### Kp

Same starting point as test_24: Kp = 3.0 µs/deg.
