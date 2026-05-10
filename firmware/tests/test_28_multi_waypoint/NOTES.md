# test_28_multi_waypoint — Notes

## Gates

| Gate | Description | Result |
|------|-------------|--------|
| GATE 1/3 | POST /mission with >=1 waypoint → mission_active, wp_count, wp_idx visible | PENDING |
| GATE 2/3 | POST /sim_gps far→near WP[0] → wp_idx 0→1, transition logged once | PENDING |
| GATE 3/3 | wp_idx reaches wp_count → mission_active=false, MISSION COMPLETE printed once; if mode=AUTO at completion, ESCs neutralized | PENDING |

## Result: PENDING (write-time commit; ready to flash and run)

## Scope of this test (read this before deciding it's worth running)

test_28 is a code-correctness sanity check, not a hardware test. No real
GPS, no real motion. We're confirming three things on the bench:

- The mission JSON parser accepts a valid array and rejects bad shapes
  without crashing.
- `wp_idx` and `mission_active` flip at the right moments (capture →
  advance, last capture → MISSION COMPLETE).
- State transitions log exactly once each (no spam, no missed prints).

The original handoff prompt's gate 4 (failsafe-during-mission preserves
mission state) was dropped: it only verifies that updateMode doesn't
touch wp_idx / mission_active, which is a code-review check rather than
something the bench can teach us. First water test (test_29) will hit
this scenario naturally if RC drops.

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
5. **3 gates instead of 5.** Handoff's "rudder commands toward WP1" gate
   is implicit (if the bearing math is broken, advancement won't fire).
   Handoff's failsafe-during-mission gate is a code-review check, not
   something the bench can teach us — dropped in favor of relying on
   test_29 to exercise it naturally.
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
5. From the repo root, run `./firmware/tests/test_28_multi_waypoint/run.sh <boat_ip>`.
   The script prompts before each step so you can verify the Serial
   line for each gate.

After gate 3 the outputs freeze at neutral; reboot to re-run.

### Optional — single-waypoint cascade

If you POST only one waypoint and capture it, gates 2 and 3 fire in the
same loop. Faster sanity loop than the full 3-waypoint flow.

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
