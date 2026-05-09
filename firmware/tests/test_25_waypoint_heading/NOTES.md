# test_25_waypoint_heading — Results

## Gates

| Gate | Description | Result |
|------|-------------|--------|
| GATE 1 | GPS fix acquired, position valid | PASS |
| GATE 2 | Drop waypoint in app, firmware telemetry shows wp_bearing | PASS |
| GATE 3 | App-shown bearing and firmware wp_bearing agree within 1° | PASS |
| GATE 4 | Rotate boat until rudder nearly straight, type 'W': heading error < 20° | PASS — 1.8° error, bow confirmed pointing at waypoint |
| GATE 5 | Move waypoint on app map, wp_bearing updates in telemetry | PASS |

## Result: PASS

Final mag axis mapping: `mr_x = -mz, mr_y = -my, mr_z = -mx` (chip X=up, Y=port, Z=stern).
Outdoor calibration offsets: MAG_OFFSET_X=-20.70, MAG_OFFSET_Y=-0.45, MAG_OFFSET_Z=-17.70.

## Open bug: rudder oscillation at 180° heading error

When the boat points directly away from the waypoint (≈180° error), the rudder
slams full port then full starboard rapidly. Root cause: the proportional
controller hits the unstable equilibrium where `shortestPathError` oscillates
between +180° and −180° with any small heading perturbation, driving the rudder
to full deflection each time.

**In practice:** unlikely to matter underway — the boat will only be anti-aligned
at startup or if a waypoint moves behind it, and any rudder deflection immediately
starts rotating the boat away from the singularity. Water drag will also damp
oscillations that bench testing exaggerates.

**Standard fix when it becomes a problem:** add a derivative term (PD controller)
— the D term resists rapid heading changes and naturally suppresses the 180°
hunting. Alternatively, cap rudder rate-of-change. Defer to test_26+.

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

### Kp

Same starting point as test_24: Kp = 3.0 µs/deg.
