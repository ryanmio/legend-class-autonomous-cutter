# test_28_multi_waypoint — Notes

## Gates

| Gate | Description | Result |
|------|-------------|--------|
| GATE 1/4 | POST /mission with >=1 waypoint → mission_active, wp_count, wp_idx visible | PENDING |
| GATE 2/4 | POST /sim_gps far→near WP[0] → wp_idx 0→1, transition logged once | PENDING |
| GATE 3/4 | wp_idx reaches wp_count → mission_active=false, MISSION COMPLETE printed once; if mode=AUTO at completion, ESCs neutralized | PENDING |
| GATE 4/4 | Failsafe round-trip during mission preserves wp_idx and mission_active | PENDING |

## Result: PENDING (write-time commit; ready to flash and run)

## What this test proves (NEW vs everything before)

1. **Multi-waypoint mission storage** — volatile array of up to 32 `{lat, lon}`
   entries. POST /mission auto-arms; POST /mission/clear empties.
2. **GPS simulation** — `POST /sim_gps {lat, lon}` overrides the BN-220 reading.
   Sticky for the session; lets bench operator step through positions.
3. **Sequencer** — when `dist(boat, waypoints[wp_idx]) < 3 m`, `wp_idx`
   advances. When `wp_idx >= wp_count`, `mission_active=false` and a single
   `MISSION COMPLETE` line is logged.
4. **Mission ↔ FAILSAFE coexistence** — failsafe pauses, doesn't reset.
   wp_idx and mission_active survive the round-trip.
5. **Telemetry exposes mission state** — `wp_idx`, `wp_count`,
   `mission_active`, `wp_dist_m`, `wp_bearing`, `gps_simulated`, `lat`,
   `lon`.

## Divergences from the handoff prompt's test_28 spec

These were intentional simplifications. Listed so the next agent knows
what was removed and why, in case any of them turn out to matter:

1. **No `/mission/start` or `/mission/stop`.** Mission auto-arms on POST.
   Re-POST to restart. Reduces endpoint surface.
2. **AUTO requires a mission.** Without an active mission OR without a
   valid position, AUTO neutralizes outputs. test_27's "AUTO = cruise +
   heading-hold-at-entry-heading" was a placeholder for a sketch with no
   mission concept — from here on AUTO means "drive the mission". This
   is also what test_29 (pool) wants.
3. **Sequencer runs regardless of mode.** Position injection advances
   wp_idx whether MANUAL or AUTO. Heading-hold + cruise only run in
   AUTO. Lets us prove the sequencer with motors silent.
4. **FAILSAFE pauses the mission.** wp_idx and mission_active survive a
   FAILSAFE round-trip. Simpler than the "resume vs restart" choice in
   the handoff.
5. **4 gates instead of 5.** Handoff's "rudder commands toward WP1" gate
   is implicit — if the bearing math is broken, advancement won't fire.
6. **All gates auto-detected.** No y/n. Per `feedback_test_style.md`.

## What this test deliberately does NOT cover

- **Cross-track guidance** — plan-test_30, separate concern.
- **PD heading tuning** — plan-test_31.
- **GPS-loss failsafe** — sea-trial territory.
- **NVS persistence** — volatile is fine; loss-on-reboot is acceptable
  for bench testing.
- **App-side multi-waypoint** — `MapScreen.tsx` is single-waypoint;
  curl proves the firmware path.

## Procedure

1. Props OFF or boat firmly secured.
2. Copy `secrets.h.example` → `secrets.h` and fill in WiFi credentials.
3. Flash. Open Serial @ 115200.
4. Power up boat, wait through 3 s ESC arming. Note the boat IP printed
   at the WiFi line.
5. **STEP 1.** POST a 3-waypoint mission. Coordinates can be arbitrary
   (sim_gps will provide the matching boat positions):
   ```
   curl -X POST http://<boat_ip>/mission \
        -H 'Content-Type: application/json' \
        -d '[{"lat":37.0001,"lon":-122.0001},
             {"lat":37.0002,"lon":-122.0001},
             {"lat":37.0002,"lon":-122.0002}]'
   ```
   → `PASS (1/4)`.
6. **STEP 2.** POST a far position then a near one to trigger advancement:
   ```
   curl -X POST http://<boat_ip>/sim_gps -H 'Content-Type: application/json' \
        -d '{"lat":37.0001,"lon":-122.0002}'
   curl -X POST http://<boat_ip>/sim_gps -H 'Content-Type: application/json' \
        -d '{"lat":37.0001,"lon":-122.0001}'
   ```
   The second POST puts the boat at WP[0], capture fires, `wp_idx 0→1`
   logged. → `PASS (2/4)`.
7. **STEP 3.** POST near WP[1] then WP[2]. Each capture logged once;
   final POST triggers `MISSION COMPLETE`. If you're in AUTO at this
   moment, ESCs are also auto-verified neutral. → `PASS (3/4)`.
8. **STEP 4 (failsafe round-trip).** POST a fresh mission, sim_gps far,
   set cruise, flip SwA AUTO, kill TX. Watch `[FAILSAFE]` line. Restore
   TX, flip SwA UP (ack), then SwA DOWN (re-engage AUTO). → `PASS (4/4)`.

After gate 4 the outputs freeze at neutral; reboot to re-run.

### Optional — single-waypoint cascade

If you POST only one waypoint and capture it, gate 2 and gate 3 fire in
the same loop (the runGates state machine handles the cascade). Useful
for a fast sanity loop before doing the full procedure.

## Telemetry shape (additions over test_27)

```
GET /telemetry  → {
  ... (test_27 fields) ...,
  "gps_valid": true,
  "gps_simulated": true,
  "lat": "37.000100",
  "lon": "-122.000100",
  "mission_active": true,
  "wp_count": 3,
  "wp_idx": 1,
  "wp_dist_m": "11.1",
  "wp_bearing": "0.0"
}
```

## /mission shape

```
POST /mission   body [{"lat":37.0001,"lon":-122.0001}, ...]
                → mission_active=true, wp_idx=0
POST /mission/clear
                → mission_active=false, wp_count=0
POST /sim_gps   body {"lat":37.0001,"lon":-122.0001}
                → gps_simulated=true (sticky for session)
```

Errors: 400 on bad JSON, missing `lat`/`lon`, empty mission, or >32
waypoints. The mission body must be a top-level JSON array (not wrapped
in `{"waypoints":[...]}`).
