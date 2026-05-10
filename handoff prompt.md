You are picking up the Legend Cutter autonomous boat project at the
doorstep of its first water test
(/Users/ryan/Documents/GitHub/legend-class-autonomous-cutter/). test_26
and test_27 are PASS. Pool test is the next milestone. Below is
everything you need.

---

## RULE ZERO — READ THE PREVIOUS TESTS BEFORE TOUCHING ANYTHING

Before you write a single line, read:

1. The .ino AND NOTES.md of the **most recent passing test** in
   `firmware/tests/`. As of 2026-05-10 that's `test_27_rc_failsafe/`.
   That sketch is the canonical base for the next two tests — copy
   from it.
2. `firmware/tests/test_25_waypoint_heading/test_25_waypoint_heading.ino`
   for the WiFi/HTTP/GPS/IMU scaffold and the haversine math.
3. `firmware/tests/test_17_manual_sailing/test_17_manual_sailing.ino`
   for canonical stick-mapping (`computeThrottleUs`, `scaledRudder`,
   diff-thrust math). Throttle stick rests at the BOTTOM (~1000 µs),
   not center.
4. `firmware/tests/test_26_mode_switch/freeze_diag/NOTES.md` — explains
   why generic "channels frozen for N ms" failsafe was the wrong
   approach AND why the no-frame timeout alone wasn't enough.
5. `firmware/FIRMWARE_AUDIT.md` — gap list for the production firmware.
6. `firmware/tests/AUTOPILOT_PLAN.md` — DRAFT plan for what comes
   next. Read its preamble before treating any specific test
   definition as instruction. The numbering in that plan does NOT
   match the actual test sequence anymore — what the plan calls
   test_29 is actually test_28 in our sequence; what the plan calls
   test_32 is test_29 (pool integration).
7. `PLANNING_NOTES.md` — recent conversation notes, water-test goals,
   open hardware issues.
8. The auto-memory at
   `/Users/ryan/.claude/projects/-Users-ryan-Documents-GitHub-legend-class-autonomous-cutter/memory/MEMORY.md`
   — pinned facts about Ryan's preferences and project state.

If you skip this and fly blind, you will burn Ryan's time and
potentially break hardware. Two recent agents got fired for exactly
that. The freeze_diag NOTES warned a third about hold-last receiver
behavior; that agent (me) read it, didn't internalize it, and shipped
a failsafe that didn't fail-safe. **Don't be that agent.**

---

## What has been proven (do not re-litigate)

- **test_07/08/15/16/17** — manual sailing closed loop: iBUS rudder +
  throttle + ESC + diff thrust + reverse interlock + failsafe.
- **test_22** — ICM-20948 hard-iron mag calibration. Offsets baked
  into test_25/26/27.
- **test_23/24** — WiFi + GPS + IMU complementary filter; PCA9685 +
  IMU on shared I2C with proportional heading hold.
- **test_25** — GPS waypoint → haversine bearing → heading hold
  rudder. PASS 2026-05-09 with 1.8° error. Single waypoint, no
  sequencer.
- **test_26** — Mode switch + autonomous throttle. PASS 2026-05-10.
  **STALE CHANNEL MAP** — its idx 5 is now the failsafe guard; its
  channel constants must NOT be lifted into new code. Banner is on
  the sketch.
- **test_27** — RC failsafe with sticky ACK + canonical Mode enum
  (MODE_MANUAL / MODE_AUTO / MODE_FAILSAFE) + HTTP `/cruise` + HTTP
  `/telemetry` + INA219 voltage telemetry. PASS 2026-05-10. **This
  is the base sketch you copy from for test_28 and test_29.**

---

## Locked channel map (2026-05-10)

| idx | CH | Use |
|---|---|---|
| 0 | CH1 | rudder (right-stick H) |
| 1 | CH2 | reverse (right-stick V down) — test_17 only |
| 2 | CH3 | throttle (left-stick V) |
| 3 | CH4 | unused |
| 4 | CH5 | knob VrA (deck gun pan / winch) |
| 5 | CH6 | **SwD failsafe guard** — sits up (~1000 µs); receiver outputs ~2000 µs as stored failsafe value on TX-loss |
| 6 | CH7 | **SwA mode switch** — up = MANUAL, down = AUTO |
| 7-9 | CH8-CH10 | unused |

Up = MANUAL because the FS-i6X forces switches up at TX power-up; that
makes MANUAL the safe boot state.

Canonical defines: `firmware/legend_cutter/config.h`. Latest test
sketch using these: `firmware/tests/test_27_rc_failsafe/test_27_rc_failsafe.ino`.
**Use those, never the test_26 constants.**

---

## Failsafe semantics (locked, inherited by every future sketch)

- **Primary trigger:** `ch[5] > 1500` sustained 500 ms → MODE_FAILSAFE.
  This is what fires on FS-iA10B because the receiver streams frames
  even with TX off but overrides CH6 with the stored failsafe value.
- **Secondary trigger (defense-in-depth):** `millis() - lastFrameMs
  >= 3000` → MODE_FAILSAFE. Doesn't fire on FS-iA10B today, but stays
  for receiver portability.
- **Behavior:** rudder + ESCs forced neutral.
- **Sticky ACK:** even after the guard clears or frames return, mode
  stays FAILSAFE until firmware sees `swcInManual()` (`ch[6] < 1450`).
  Operator must flip SwA UP to acknowledge before AUTO can re-arm.
- **Override direction is one-way:** firmware can only *downgrade*
  mode (AUTO → MANUAL/FAILSAFE), never *upgrade*. Operator MANUAL is
  always honored; operator AUTO is conditional.
- **SwD operator rule:** never deliberately flick SwD — it's the
  guard. Flicking it down trips MODE_FAILSAFE by design.

---

## What Ryan needs you to do next

Two sketches in this order. Both inherit from test_27.

### test_28 — multi-waypoint mission (BENCH-ONLY, GPS-spoofed)

Ryan's yard isn't big enough to walk a 1.3 m boat through waypoints,
and he won't take it to a public park. So this test does NOT use real
GPS motion — it uses HTTP-injected fake GPS positions to step the
boat through a mission on the bench.

**Goal:** prove the multi-waypoint sequencer (storage → mission entry
→ advancement on capture → mission end) without any physical motion.

**What's NEW vs test_27:**

- Mission storage. Volatile is fine for v1 (no NVS persistence
  required). Array of up to ~32 `{lat, lon}` entries plus `wp_idx`
  and `wp_count`.
- HTTP endpoints:
  - `POST /mission` — body is a JSON array of `{lat, lon}` objects;
    replaces current mission, resets `wp_idx = 0`.
  - `POST /mission/clear` — empties the mission.
  - `POST /mission/start` — arms the sequencer.
  - `POST /mission/stop` — disarms.
  - `POST /sim_gps {lat, lon}` — overrides the real GPS reading.
    The sequencer (and heading hold) consumes this position as if
    it came from the BN-220. **This is the bench-test injection
    point.** Default: if `/sim_gps` has been called this session,
    use that; otherwise use the real GPS.
- Telemetry adds: `wp_idx`, `wp_count`, `wp_dist_m_to_active`,
  `wp_bearing_to_active`, `mission_active`, `gps_simulated`.
- Sequencer logic: when `dist(boat, waypoints[wp_idx]) < CAPTURE_RADIUS_M`
  (3 m), advance `wp_idx`. When `wp_idx >= wp_count`, set
  `mission_active = false`, drop ESCs to neutral, log `MISSION
  COMPLETE` to Serial **once**.

**Pass gates (all from the bench, ~10 minutes total):**

1. POST a 3-waypoint mission. `/telemetry` shows `wp_count=3,
   wp_idx=0`.
2. POST `/sim_gps` with a coordinate ~10 m from WP1.
   `wp_dist_m_to_active` ~10. Rudder commands a heading toward
   WP1 (re-uses test_25's logic — already proven).
3. POST `/sim_gps` near WP1. `wp_idx` advances 0→1 within one loop
   tick. Serial prints one transition line.
4. POST near WP2 → wp_idx 1→2. POST near WP3 → wp_idx 2→3. Serial
   prints `MISSION COMPLETE` once. ESCs neutralized.
5. RC failsafe still works during a mission (kill TX → guard trips
   → MODE_FAILSAFE, mission paused; recovery via SwA flip resumes
   or restarts depending on design choice — pick the simpler
   behavior and document).

**Don't add:** cross-track guidance (plan-test_30, separate concern),
PD heading tuning (plan-test_31), GPS-loss failsafe (sea-trial
territory).

App-side note: `app/src/screens/MapScreen.tsx` currently has
single-waypoint POST. Multi-waypoint extension is small but not
required for test_28 — `curl` of a JSON array proves the firmware
path; app integration follows separately.

### test_29 — pool integration (FIRST WATER TEST)

This is the real one. Read `AUTOPILOT_PLAN.md`'s test_32 section
before starting — it documents the pool success criteria. Note the
plan numbers it test_32; in our actual sequence it's test_29.

**Purpose:** integrate everything proven so far into a single sketch
Ryan can flash and take to the pool. Manual control + AUTO
single-waypoint + RC failsafe + voltage telemetry, all in one. The
multi-waypoint sequencer from test_28 may or may not be included —
it's a *bonus* per the plan, not a success criterion.

**What this sketch is:** a careful merge of test_27 (Mode + failsafe
+ /cruise + /telemetry) with test_25 (HTTP `POST /waypoint`,
haversine bearing, heading hold). Add: capture-radius detection that
drops ESCs to neutral when `wp_dist < 3 m` (mission complete).
That's the only new logic.

**Pool success criteria (from AUTOPILOT_PLAN.md, the bar Ryan cares
about):**

1. Watertight ≥30 min.
2. Manual TX control works (rudder + throttle).
3. Mode switch + telemetry observable on the phone.
4. AUTO single-waypoint to within 3 m capture radius.
5. TX-off → boat stops within 4 s.
6. No motor/ESC weirdness, no battery surprises, no obvious
   heading drift.

A 3-waypoint mission completion is a *bonus*. If test_28 has landed
by the time we're at the pool, include the mission sequencer; if
not, ship single-waypoint.

**Pre-pool checklist (BEFORE GETTING WET):**

- Bilge MOSFET fix verified (Ryan working on it 2026-05-10 evening
  — see PLANNING_NOTES.md and the proposed `test_BILGE_DIAG`
  outline in `AUTOPILOT_PLAN.md:247`).
- Hatch waterproofing verified (Ryan, same evening).
- All four test_27 gates pass on a fresh flash of the integration
  sketch (not just test_27 in isolation — the merge can break
  things).
- PID gains conservative (Kp=2-3, Kd=0.5-1, Ki=0; tune from
  test_25 + test_27 baseline).
- Cruise set conservatively via `/cruise` (e.g., 1660-1700 µs to
  start; in water with drag, may need higher — let Ryan iterate).
- Phone connected, `/telemetry` visible at ≥1 Hz.
- SwA at MANUAL boot position. SwD at up.
- Props on, rudder linkage free, hatch sealed.

**Pool test sequence (per the plan):**

1. Manual hover ~5 min. Confirm RC, sticks, no leaks.
2. Heading hold static — point at one waypoint, flip to AUTO at
   cruise=neutral. Rudder should track waypoint regardless of
   small disturbances.
3. Single-waypoint AUTO at cruise=30%. Boat drives to waypoint,
   stops within 3 m capture.
4. (Bonus) 3-waypoint mission if test_28 has landed.
5. Failsafe live test — start AUTO, kill TX, confirm boat stops
   within 4 s.

---

## Hard rules — never violate

- **Serial output discipline.** No streaming, no per-second prints,
  no hint-repeat timers. Each prompt prints **once** on phase entry.
  Mode/state transitions print once on the transition (latch the
  print). "Refusal" / "ack-required" / "still-waiting" messages:
  latch with a `static bool`, only print on the rising edge. Ryan
  wants scrollable Serial output, not flooded. **This rule has been
  violated repeatedly — see `feedback_serial_output.md` in memory
  for the full history of why he's tired of restating it.**
- **Tests prove only NEW capability.** Don't re-walk gates that
  earlier tests already PASSed. Read prior NOTES.md, isolate the one
  new thing, design gates that test ONLY that.
- **Sketches auto-detect gates.** Operator moves things; sketch
  prints `PASS (N/M)` when conditions are met; sketch prompts the
  next step. No "type M to print mode state" debug commands as
  primary flow.
- **Stick conventions:** throttle rests at the BOTTOM (~1000 µs);
  right stick is spring-centered; safe-arm checks throttle at IDLE
  not center.
- **Stale channel maps are stale.** Use the locked map above.
  test_26's channel constants are wrong for everything
  post-2026-05-10.
- **Commit per test.** Write-time commit when you write the sketch,
  PASS commit when Ryan confirms gates.
- **GPS wiring:** BN-220 white = TX (reversed), green = RX. ESP32
  RX → white. GPS_RX_PIN=17, GPS_TX_PIN=4.
- **Audio is DF1201S (DFPlayer Pro)** at 115200 baud, AT protocol
  — not the DFPlayer Mini. `legend_cutter/audio.cpp` still needs
  porting. Defer post-pool.
- **No water testing until bench validation is complete.**
- **Ryan is a Mode-2 RC pilot.** Right stick H = rudder = CH1.

---

## Open hardware issues (parallel tracks, don't block test_28)

1. **Bilge MOSFET module misbehaving.** Floating logic ground, LED
   on without command, pump activates on touch, ESC chirped weird
   while it was wired. Currently disconnected. Ryan tackling
   2026-05-10 evening. Blocks water testing, not bench autopilot
   work. Diagnostic outline in `AUTOPILOT_PLAN.md`.
2. **DF1201S audio went silent** during recent unrelated testing.
   Defer post-pool. Cosmetic.
3. **iBUS link ~1% checksum loss baseline** (test_15/16). Not
   blocking; characterize further if it gets worse under motor load.

---

## Memory files (auto-loaded into your context)

`/Users/ryan/.claude/projects/-Users-ryan-Documents-GitHub-legend-class-autonomous-cutter/memory/MEMORY.md`
indexes user-level facts and feedback. Among them:

- `feedback_serial_output.md` — serial discipline (read it).
- `feedback_test_style.md` — auto-detect gates, terse procedures.
- `feedback_test_novelty.md` — only test what's new.
- `feedback_commit_per_test.md` — commit rhythm.
- `project_stick_conventions.md` — bottom-rest throttle.
- `project_imu_axis_mapping.md` — confirmed mag axes.
- `project_bn220_wire_colors_reversed.md` — GPS wiring quirk.
- `project_audio_module_df1201s.md` — DF1201S not Mini.
- `project_deck_gun_pan_continuous.md` — pan servo type/channel.
- `user_tx_mode.md` — Mode 2 pilot.

---

## What Ryan needs from you

Get him to the pool. test_28 (bench, GPS-spoofed multi-waypoint) is
the next focused test; test_29 (pool integration) is the one he
flashes for the water. Multi-waypoint can ship in either or be
deferred entirely — single-waypoint AUTO is what the pool success
criteria require.

He has had a long road through this project and has been burned by
agents who hand-waved through safety-critical gates, polluted Serial
with hint-repeat timers, and missed receiver-behavior subtleties
documented right next to the code. Be precise, be brief, and read
code before writing it. Acknowledge mistakes directly when called
out — don't pretend they weren't real. If you don't know something,
say so before writing code that depends on the assumption.

The next milestone is the pool test. Get him there.
