# test_29_pool_integration — Notes

## Status: POOL-VERIFIED 2026-05-30 (run #2, firmware `test_29-pool2`); cal procedure reworked + true-heading fixes in `test_29-pool2.6-magcal2` (UNTESTED, MC2 gates pending — recal required after flash); lake test next

## What this sketch is

Production-direction sketch for the **first water test**. Not a bench
gate test — there are no auto-detected gates. The operator drives via
the app; the sketch operates.

Merges:
- **test_27** Mode FSM, CH6 SwD failsafe guard with sticky ACK, /cruise,
  /telemetry, INA219 voltage telemetry.
- **test_25** Single /waypoint + haversine bearing + heading-hold.
- **NEW** Capture detection (sticky), live /pid tuning. (/sim_gps
  existed through pool2.4 but was removed 2026-06-10 — see below.)

## What's NEW vs everything before

1. **POST /waypoint** — `{lat, lon}` arms a single waypoint; `{lat:null,
   lon:null}` clears. Already wired in `app/src/screens/MapScreen.tsx`.
2. **Capture detection** — sticky `captured` flag fires on EITHER:
   - **DISTANCE:** `wp_dist < 3 m` (the existing trigger).
   - **CROSSING:** boat passes the perpendicular line through the
     waypoint, perpendicular to the leg from start→waypoint. Start
     point = boat position on the first GPS fix after AUTO engage
     (changed 2026-06-10; was first fix after `/waypoint` POST).
     Stops the boat circling forever when GPS noise (~2-3 m) is
     close to the capture radius (3 m). Whichever fires first wins.
   Capture detection only arms while in AUTO (changed 2026-06-10) —
   driving past the waypoint manually can no longer mark the leg done.
   ESCs + rudder forced neutral on capture. Serial logs which trigger
   fired (`captured by DISTANCE` vs `captured by CROSSING`).
3. **POST /pid {kp, kd}** — live heading-hold tuning during the run.
   Defaults Kp=3.0, Kd=8.0 (test_27/test_28 baseline). Ki=0 — pool
   tuning is P+D only.
4. **Cruise floor refusal removed** — cruise=NEUTRAL_US (1500) is now
   valid. Static-heading-hold scenario from AUTOPILOT_PLAN test_32
   step 2 just works.
5. **AUTO without a waypoint = neutral** — test_27's "AUTO holds
   heading at entry, runs cruise" placeholder is replaced. From here,
   AUTO means "drive to the active waypoint" or stay neutral.
6. **MANUAL reverse** (CH2, ported from test_17). Throttle stick at
   idle + right-stick V pulled down past the deadband → ESCs run
   reverse, capped at MIN_REV_US=1200 (~60% reverse). Forward throttle
   always wins — the interlock blocks reverse while the left stick is
   above THROTTLE_IDLE_MAX, so you can't slam forward→reverse instantly.
   AUTO mode never asks for reverse. Telemetry exposes `ch_reverse`.
7. **POST /led + /audio** — HelmScreen light toggles (nav / bridge /
   deck on GPIO 18 / 19 / 23) and the three sound buttons. Audio
   plays by **index** via `playFileNum()` (AT+PLAYNUM under the
   hood); path-based playback (`playSpecFile`/AT+PLAYFILE) is broken
   in DFRobot firmware ([Issue #5](https://github.com/DFRobot/DFRobot_DF1201S/issues/5):
   chip silently falls back to file 1 on any mis-resolution, exactly
   what we hit on the first water test). Index map: horn → 1, gun →
   2, board → 3. Indices reflect FAT **write order**, which is fixed
   deterministically by `audio-assets/dfplayer/load.sh` — run that
   script once after plugging the DF1201S in via USB to load the
   tracks in the canonical order. Telemetry exposes `nav_on`,
   `bridge_on`, `deck_on`, `audio_ok` so the app can reconcile its
   optimistic state and surface "audio dead" without USB.

## Serial architecture (non-obvious)

ESP32 has 3 hardware UARTs and we have 4 serial peripherals (USB
Serial / iBUS / GPS / DF1201S). Allocation:

| UART | Peripheral | Baud |
|------|-----------|------|
| UART0 (USB)       | debug Serial                 | 115200 |
| UART1 (GPIO 16)   | iBUS RX-only                 | 115200 |
| UART2 (GPIO 25/26) | **DF1201S audio**           | 115200 |
| SoftwareSerial (GPIO 17/4) | **GPS BN-220**       | 9600   |

GPS displaced to SoftwareSerial because DF1201S needs 115200 and
SoftwareSerial-at-115200 is unreliable on ESP32 with WiFi active
(empirically confirmed 2026-05-16 — begin() always timed out).
SoftwareSerial-at-9600 is well within library limits and GPS NMEA is
checksum-protected so dropped bytes just mean dropped sentences, not
corrupted fixes. **Don't move GPS back to UART2 without moving
DF1201S off it.** Requires the `EspSoftwareSerial` library.

## Bilge pinout

| GPIO | Role | Wiring |
|------|------|--------|
| 13 | BILGE_PUMP (MOSFET gate, active HIGH) | signal → MOSFET gate; pump is 14 V from battery |
| 32 | BILGE_FWD_SENSOR (forward compartment, active LOW) | probe pair: one to GPIO 32, one to GND. Internal pullup. |
| 33 | BILGE_MID_SENSOR (main bilge at pump, active LOW) | same wiring as fwd |
| 5  | BILGE_REAR_SENSOR (rear compartment, active LOW) | same wiring as fwd. GPIO 5 is a strapping pin (SDIO slave mode) but only matters for that boot path — safe as a sensor input. |
| 2  | RADAR_MOTOR (NPN base, PWM via LEDC ch 0 @ 20 kHz) | ESP32 GPIO 2 → 1 kΩ → 2N2222 base; 3.3 V → motor → collector; emitter → GND. Speed 0-100% mapped to 8-bit duty. **No flyback diode currently — back-EMF will eventually degrade the transistor under sustained PWM. Add 1N4148/1N4001 anti-parallel across the motor before extended use.** Strapping pin (download mode) but only matters when GPIO 0 is LOW. Onboard dev-board LED tracks PWM (appears dim at low duty). |
| 27 | SONAR_TRIG (RCWL-1655 trigger, board label `TRIG_RX_SCL_I/O`, 10 µs HIGH pulse) | ESP32 GPIO 27 → sonar TRIG. **Pin labels on the RCWL-1655 board are non-standard** — verify against the silkscreen, not HC-SR04 conventions. |
| 14 | SONAR_ECHO (RCWL-1655 echo, board label `ECHO_TX_SDA`, pulse-width = distance) | sonar ECHO → ESP32 GPIO 14, INPUT_PULLDOWN. Sensor runs on 5 V; if echo readings are flaky add a 1 kΩ + 2 kΩ divider. `pulseIn()` blocks up to 40 ms per ping. Range 2-450 cm. |

Pump control loop runs every loop() iteration:
- `wet = (fwd LOW) || (mid LOW) || (rear LOW)`
- pump auto-on while wet AND for 5 s after the last wet reading (dry-delay
  prevents short-cycle on intermittent splash)
- `POST /bilge {on:true}` forces pump; auto-clears 60 s later (safety
  against forgotten "on" draining battery)

## What's deliberately NOT here

- Multi-waypoint missions (pool too small per `project_pool_environment`
  memory; bay/lake scope).
- Cross-track guidance (plan-test_30).
- Integral term (plan-test_31; pool is P+D).
- GPS-loss failsafe (sea-trial scope).
- /estop, /rth, /set-home, /mode (RC failsafe + SwD physical guard
  cover the safety case for pool; production firmware adds these).

## HTTP API

| Method | Path        | Body                           | Effect |
|--------|-------------|--------------------------------|--------|
| GET    | /status     |                                | `{ok, v, ip}` |
| GET    | /telemetry  |                                | full JSON (see below) |
| POST   | /cruise     | `{us:1660}` or `{pct:50}`      | sets AUTO cruise µs |
| POST   | /waypoint   | `{lat, lon}` or `{lat:null,lon:null}` | arms / clears waypoint. Rejects (400) waypoints >1000 m from the boat when a fix exists; otherwise auto-clears once the first fix shows the distance. On set: `captured=false`; leg-start is recorded on the first fix after AUTO engage. |
| POST   | /pid        | `{kp, kd}` (either or both)    | live tuning |
| POST   | /led        | `{light:"nav"\|"bridge"\|"deck", state:bool}` | toggles nav (GPIO18) / bridge (GPIO19) / deck (GPIO23) |
| POST   | /audio      | `{sound:"horn"\|"board"\|"gun"}` | horn → `playFileNum(1)`, gun → `playFileNum(2)`, board → `playFileNum(3)`. Index map depends on FAT write order — set by `audio-assets/dfplayer/load.sh`. 400 on unknown sound, 503 if DF1201S didn't ACK at boot. |
| POST   | /bilge      | `{on:bool}` | manual pump override. `on:true` forces pump for up to 60 s (auto-clears); `on:false` releases. Auto-pump-on-leak continues to fire regardless. |
| POST   | /radar      | `{on?:bool, speed?:0..100, burst_ms?:2..5000, pause_ms?:0..60000}` | mast radar dish motor. Burst-only — PWM at `speed` for `burst_ms`, off for `pause_ms`, repeating. Fakes slow rotation from a too-fast geared motor (smooth-PWM mode removed 2026-05-20 because actual motor is ~2000 RPM peak and smooth at any duty looked like a propeller). Defaults: speed=25, burst_ms=3, pause_ms=200 → ~36° step at ~5 steps/sec ≈ radar-look. `on:false` cuts output. |
| POST   | /depth      | `{mode:"stop"\|"check"\|"run"}` | RCWL-1655 sonar. `run` pings every 20 s, `check` takes a one-shot reading (mode unchanged), `stop` halts polling AND clears `depth_m` from telemetry. Last reading persists in firmware across CHECK→RUN transitions; only STOP wipes it. Conversion uses freshwater sound speed (13.4 µs/cm). **Bench tests in air read ~4.3× too large** (sound is slower in air) — sanity-check accordingly. |

## Telemetry shape

```
{
  "v":            "test_29",
  "uptime":       int seconds,
  "heap":         int bytes,
  "mode":         "MANUAL" | "AUTO" | "FAILSAFE",
  "cruise_us":    1500..1800,
  "failsafe_ack": bool,
  "rc_ever_good": bool,
  "rc_age_ms":    int,
  "rudder_us":    1330..1670,
  "esc_us":       1500..1800,
  "ch_throttle":  raw iBUS,
  "ch_rudder":    raw iBUS,
  "ch_mode":      raw iBUS,
  "ch_guard":     raw iBUS,
  "nav_on":       bool,
  "bridge_on":    bool,
  "deck_on":      bool,
  "audio_ok":     bool,    // true if DF1201S ACKed at boot; /audio returns 503 when false
  "bilge_fwd":    bool,    // forward compartment sensor wet
  "bilge_mid":    bool,    // main bilge sensor wet (at pump)
  "bilge_rear":   bool,    // rear compartment sensor wet
  "pump":         bool,    // pump MOSFET currently on
  "pump_manual":  bool,    // operator forced via /bilge (auto-clears 60 s after last on)
  "pump_stuck":   bool,    // auto-pump latched off after 60 s continuous run (sensor likely stuck wet)
  "radar_on":       bool,    // mast radar dish motor on/off
  "radar_speed":    int,     // 0..100 PWM duty during burst phase
  "radar_burst_ms": int,     // burst ON phase length (live-tunable)
  "radar_pause_ms": int,     // burst OFF phase length (live-tunable)
  "depth_mode":     "off" | "run",
  "depth_m":        "X.XX"   (if a reading exists; absent after STOP or pre-first-ping),
  "depth_age_ms":   int      (ms since last successful ping; absent if never read),
  "heading":      "0..360",
  "batt_v":       "X.XX"   (if INA219 present, volts),
  "batt_a":       "X.XX"   (if INA219 present, amps),
  "gps_fix":      bool,
  "lat":          "X.XXXXXX" (if gps_fix),
  "lon":          "X.XXXXXX" (if gps_fix),
  "sats":         int,
  "speed_kts":    "X.X"     (if GPS reports it),
  "course":       "X.X"     (if GPS reports it),
  "wp_set":       bool,
  "captured":     bool,
  "wp_lat":       "X.XXXXXX" (if wp_set),
  "wp_lon":       "X.XXXXXX" (if wp_set),
  "wp_dist_m":    "X.X"      (if wp_set + gps_fix),
  "wp_bearing":   "X.X"      (if wp_set + gps_fix),
  "wp_start_lat": "X.XXXXXX" (if wp_set + leg-start recorded),
  "wp_start_lon": "X.XXXXXX" (if wp_set + leg-start recorded),
  "captured_by":  "none" | "distance" | "crossing",   // which trigger fired
  "pid_kp":       "X.XX",
  "pid_kd":       "X.XX"
}
```

## Pre-flight checklist (before getting wet)

- [x] Bilge MOSFET fix verified (signal wire 15 → 13; radar 13 → 15; 2026-05-12).
  Boat-side check: LED off + pump silent at idle, ESP32 boots normally.
- [ ] Hatch waterproofing verified.
- [ ] Manual TX control: rudder + throttle work on the bench.
- [ ] SwA at MANUAL boot position. SwD at up.
- [ ] Phone connected to same network as boat. App opened.
- [ ] `/telemetry` visible in app. Voltage reading sane.
- [ ] PID live-set to conservative values (Kp=3.0, Kd=8.0 default is fine).
- [ ] Cruise set low to start (1660 default; iterate).
- [ ] Props on, rudder linkage free, hatch sealed.
- [x] HelmScreen LED toggles confirmed (nav / bridge / deck) — 2026-05-16.
- [x] HelmScreen sound buttons confirmed (track 1 plays) — 2026-05-16.
- [x] GPS still gets a fix on SoftwareSerial after the UART swap — 2026-05-16.
- [ ] All 3 bilge sensors trigger pump on contact with a wet rag at the probe pads. PUMP turns on. Pump stops 5 s after rag removed. Damage-control panel in SystemsScreen lights the matching zone red.
- [ ] Manual PUMP button in SystemsScreen turns pump on (MANUAL indicator) and then releases on second tap.
- [ ] Radar 25/50/75/100 presets in SystemsScreen each set a visibly different dish speed. OFF stops it. Onboard ESP32 LED brightness tracks duty.
- [ ] DEPTH CHECK in SystemsScreen returns a reading against the bench (hold sonar over a known distance). **Expect ~4.3× the real distance in air** (e.g., 30 cm wall → ~130 cm reading) because conversion uses freshwater sound speed. RUN polls every 20 s, value also visible on HelmScreen. STOP clears the reading.

## Pool sequence (per AUTOPILOT_PLAN test_32)

1. **Manual hover** ~5 min. Confirm RC, sticks, no leaks.
2. **Heading hold static.** Drop a waypoint on the map nearby. Set
   cruise to 1500 (neutral). Flip SwA AUTO. Boat doesn't move; rudder
   should track waypoint as you spin the boat by hand.
3. **Single-waypoint AUTO at cruise.** Set cruise to ~1660. Drop
   waypoint a few metres ahead. Flip AUTO. Boat drives, captures within
   3 m, ESCs neutralize.
4. **Failsafe live.** Start an AUTO run. Kill TX. Boat should stop
   within ~4 s (CH6 guard 500 ms detect + processing). Restore TX,
   flip SwA UP to ACK, then DOWN to re-engage. wp_idx survives the
   cycle.

A 3-waypoint mission is NOT in scope per recent conversation
(2026-05-10) — the pool is too small.

## Pool run 2026-05-29 — partial PASS, AUTO blocked

Ran ~10 minutes in the backyard pool. Handling overall: very pleased.
Hull integrity good — after 10 min the only water inboard was a small
amount in the rear compartment (expected). No leaks elsewhere. Depth
sensor returned accurate readings.

Two blocking issues found before AUTO could be tested:

1. **ESC direction is inverted.** Stick-forward (CH3 up) commands the
   props in reverse; stick-down commands forward. AUTO would have driven
   full-speed reverse with no steering authority, so AUTO leg was
   skipped. Needs fix before next pool run — either swap the ESC
   forward/reverse mapping in `motors.cpp` or reverse the throttle sense
   feeding `setEscs()`. (Whichever keeps AUTO's "forward = MAX_FWD_US"
   semantics intact.)

2. **Boat is far too fast even with current limits.** Running manual
   reverse at an estimated 30–50% stick, the bow wake climbed above the
   deck and threw water out to the sides. Awesome in a pool; in any
   chop the boat would submarine itself. Current throttle cap needs to
   come down significantly.

### Current throttle limits (config.h)

- `NEUTRAL_US = 1500`
- `MAX_FWD_US = 1800` → forward swing is 300 µs of the 500 µs full
  range = **60% of full forward throttle**.
- `MIN_REV_US = 1200` → reverse swing 300 µs = **60% of full reverse**.
- `AUTO_CRUISE_CAP_US = 1750` (50 µs below MAX_FWD = ~50% of full).
- `DEFAULT_CRUISE_US = 1660` (~32% of full forward).

So manual is already capped at 60% and that was still problematically
fast. Next pool run should drop MAX_FWD_US / MIN_REV_US substantially
(candidate: ~1650 / ~1350, i.e. ~30% each way) and re-pick
AUTO_CRUISE_CAP_US / DEFAULT_CRUISE_US accordingly. Do not change yet —
pending decision.

### Status

- [x] Manual handling, hull seal, depth sensor — PASS
- [ ] ESC direction fix — required before re-test
- [ ] Throttle cap reduction — required before re-test
- [ ] AUTO heading hold / waypoint capture — DEFERRED to next pool run
- [ ] Failsafe live — DEFERRED to next pool run

## Pool run #2 2026-05-30 — PASS, ready for lake test

Firmware: `test_29-pool2` (commit `99b48ac3`). All pool-2 fixes
validated — boat drives great.

### Confirmed PASS

- **ESC direction fix** — forward stick = forward props. `ESC_DIRECTION_INVERTED`
  mirror at the PCA write boundary works as intended. Diff thrust and
  AUTO yaw direction also correct (verified during AUTO test below).
- **Throttle trim to ~42% (MAX_FWD_US=1710 / MIN_REV_US=1290)** — speed
  feels right. Boat is responsive but no longer submarining itself at
  manual full stick. Cap can be revisited after the lake run; for now
  it's a keeper.
- **AUTO mode engages and drives** — flipped SwA, boat moved toward the
  posted waypoint and yawed via differential thrust.

### Open issue — IMU heading appears ~20° off

When a waypoint was set and AUTO engaged, the boat turned in roughly
the right direction but settled pointing ~20° to the LEFT of the
intended bearing. Repeated with a waypoint in a different direction —
same ~20° leftward offset. Suggests a systematic IMU heading bias
rather than a navigation-math bug.

Pool was too small to see whether GPS course-over-ground would have
corrected the heading estimate over a longer run (the existing IMU+GPS
fusion is supposed to nudge IMU heading toward GPS course once the
boat is moving consistently). At pool speeds and distances the GPS
correction loop probably never got enough signal to engage.

Likely fix: **magnetometer calibration on the assembled boat**, with
all the metal/motors in their final positions (different magnetic
environment than wherever the IMU was last calibrated). Worth doing
before the lake run.

### Status

- [x] ESC direction fix — PASS
- [x] Throttle cap (1710/1290/1700/1620) — PASS
- [x] AUTO engages and drives toward waypoint — PASS
- [x] Differential-thrust yaw direction correct — PASS
- [x] Hull seal at ~10 min — PASS (run #1 already confirmed; run #2 consistent)
- [~] AUTO heading accuracy — ~20° leftward bias; investigate IMU mag cal before lake
- [ ] GPS+IMU fusion behavior over distance — needs lake (pool too small)
- [ ] Failsafe live — not exercised this run; defer to lake
- [x] App auto-flight-logging (commit `2b4bbc10`) — PASS in pool run #2: logger auto-started on throttle-up, flight saved to FlightsScreen, tap-to-share produced CSV via system share sheet

### Next action: lake test

Pre-lake checklist:
1. Run magnetometer calibration with the boat fully assembled, motors
   in place. Verify heading reads true on all four cardinal directions
   at rest.
2. Bench-test the app auto-flight-logger against the new firmware
   (Expo Go) before relying on it for the lake run.
3. Confirm the app's Firmware row shows `test_29-pool2.1-magcal`
   (post-magcal integration) or later.
4. Lake run: longer AUTO leg so GPS-course fusion has time to nudge
   IMU heading. Watch whether the ~20° offset closes on its own.

## Mag-cal integration 2026-05-30 — bench-the-boat gates pending

`test_29-pool2.1-magcal` adds NVS-backed mag calibration with safe
fallback. Pre-cal behaviour is unchanged: NVS empty → existing
hardcoded `DEFAULT_MAG_OFFSET_*` defaults remain in use.

New surface:
- `POST /calibrate_mag/start` — boat enters cal mode, spin operator
  rotates whole boat through 360° on a flat surface. Plateau detected
  → offsets computed → saved to NVS namespace `imu_cal` → applied to
  live heading. 60 s timeout.
- `POST /calibrate_mag/abort` — return to idle without saving.
- Telemetry adds: `mag_cal_state`, `mag_cal_progress`,
  `mag_calibrated`, `mag_off_x/y/z`, `mag_baseline_uT`, `mag_uT`,
  `mag_from_nvs`, `mag_cal_ts`, optional `mag_cal_fail`.

### Bench-the-boat gates (need PASS before lake)

| Gate | What |
|------|------|
| MC-1 | Flash `test_29-pool2.1-magcal`. Boat boots, app `Firmware` row reads `test_29-pool2.1-magcal`. |
| MC-2 | Fresh-NVS boot (no prior cal): Serial shows `[mag-cal] NVS empty, using hardcoded defaults`. `/telemetry` shows `mag_calibrated=false`, `mag_from_nvs=false`, offsets match the hardcoded defaults. |
| MC-3 | All other systems still work — AUTO, manual, bilge, depth, GPS, etc. (Smoke-test, not a full pool rerun.) |
| MC-4 | `curl -X POST http://<ip>/calibrate_mag/start` → `mag_cal_state` transitions to `"collecting"`. `mag_cal_progress` climbs as operator rotates whole boat. |
| MC-5 | Within 60 s of rotation: `mag_cal_state="done"`, `mag_off_x/y/z` populated with new values, `mag_baseline_uT > 20`. |
| MC-6 | Power-cycle ESP32. On reboot: Serial shows `[mag-cal] loaded from NVS`. `/telemetry` shows `mag_calibrated=true`, `mag_from_nvs=true`, same offsets. |
| MC-7 | After cal: app's `Heading` row matches a known compass bearing ±15° (use phone compass app to find true bearing). |
| MC-8 | Two consecutive heading readings with boat still differ < 5°. |
| MC-9 | `POST /calibrate_mag/abort` during a `collecting` run → state returns to `idle`; NVS unchanged. |

MC-7 and MC-8 are the gates test_22 documented but never formally
completed. Don't skip them — these are the proof the cal procedure
actually fixes the pool-#2 ~20° AUTO heading bias.

## Autonomy-review fixes 2026-06-10 — `test_29-pool2.5` (UNTESTED)

Code-review pass after the 2026-06-05 harbor run. Five firmware changes,
one commit each:

1. **WiFi reconnect is non-blocking.** The old loop-side reconnect ran a
   blocking scan + up to 20 s of connect polling — during which iBUS,
   the mode FSM, failsafe detection, and outputs were all frozen at
   their last values. A WiFi drop mid-AUTO meant the boat kept driving
   blind for ~25 s with both failsafes dead. Now: WiFi loss changes
   nothing — AUTO runs on RC + GPS alone; `wifiMaintain()` re-issues a
   non-blocking `WiFi.begin()` (boot-time credentials, no scan) at most
   every 30 s. Blocking scan-first connect still used in `setup()` only.
2. **`/sim_gps` removed** (firmware + app traces). It was sticky for the
   session with no clear path and no interlock — one accidental POST in
   water would freeze the boat's believed position, making both capture
   triggers unable to ever fire while AUTO drove on a frozen bearing.
   Bench dry-runs of the app now require real GPS.
3. **Waypoint fat-finger guard** — `/waypoint` rejects targets >1000 m
   from the boat (`MAX_WP_DIST_M`); if no fix exists at POST time, the
   waypoint is auto-cleared at the first fix that shows it out of range.
4. **Heading-hold D-term timing fix** — the damping term was sampled at
   loop rate (~1-5 ms) against a heading that updates every 20 ms,
   producing dErr spikes ~10× too large interleaved with zeros. The turn
   rate (`headingRateDps`) is now computed once per IMU update. Expect
   smoother rudder in AUTO; Kd may want re-tuning since its effective
   authority changed.
5. **Capture arms only in AUTO** — `captured` can no longer trip while
   driving manually past the waypoint; leg-start (crossing-line anchor)
   is recorded at AUTO engage, not at `/waypoint` POST.

Also: stale SAFETY header / `/cruise` error text corrected to match the
1800 µs uncap (no behavior change).

### Heading reliability — analysis of 2026-06-05 harbor log

**(Conclusion superseded — see the 2026-06-12 corrected analysis below.
The motor/ESC attribution was wrong.)**

`mission_logs/2026-06-05T15-26-05.csv` (595 rows): compass heading vs
GPS course-over-ground while moving >0.8 kts (254 samples) shows
median |error| ~36°, consistent +23° bias, stdev 39°. Live `mag_uT`
swung 13.6–73.7 against a 27.7 baseline — the magnetic field at the
sensor changes while driving (motor/ESC current), which static cal
cannot fix. Conclusion: mag-only heading is not trustworthy under
power. Future direction (deferred): blend GPS course into the heading
estimate when speed is sufficient.

AUTO during that log: ESCs held 1632 µs for all 58 AUTO seconds,
speed 0–0.5 kts — confirms the "AUTO didn't move the boat" report;
thrust at that level is insufficient in open water.

### Bench-the-boat gates (before next water run)

| Gate | What |
|------|------|
| AR-1 | Flash `test_29-pool2.5`. App Firmware row reads `test_29-pool2.5`. All prior behavior unchanged (manual drive, AUTO engage, bilge, depth, radar, audio smoke-test). |
| AR-2 | WiFi drop: with RC live, kill the hotspot. RC control stays perfectly responsive (no freeze, no stutter). Restore hotspot — telemetry returns within ~30 s without reboot. |
| AR-3 | `/sim_gps` returns 404. Telemetry has no `gps_simulated` field. |
| AR-4 | `/waypoint` with a point >1000 m away returns 400 and `wp_set` stays false. |
| AR-5 | Set a waypoint, drive past it MANUALLY — `captured` stays false. Flip AUTO near it — capture fires normally. |
| AR-6 | (Water) AUTO rudder visibly smoother than pool2.4; re-tune Kd if needed. |
| AR-7 | (Water, operator) TX failsafe live: kill TX mid-AUTO, boat stops within ~4 s. Verifies the RX actually signals loss (known failsafe value on CH6 per operator). Restore TX, SwA UP to ACK. |

## Heading root cause — corrected analysis 2026-06-12

Re-analysis of the same 2026-06-05 harbor log, fitting heading−course
error against course (the classic compass deviation curve, 160 samples
> 1 kt):

- **One-cycle sinusoid, 42° amplitude** — textbook residual hard-iron.
  Error runs +60° heading north → −35° heading west. That implies
  ~14 µT of leftover horizontal offset against the local ~20.5 µT
  horizontal field, i.e. the cal itself was bad.
- **Constant term +13.9° ≈ local declination (~11° W)** — the firmware
  never converted magnetic heading to true, but steers toward true GPS
  bearings. That alone is most of pool run #2's "~20° leftward" bias.
- **Motor/ESC effect is minor**: |Δmag| at 15 throttle transitions with
  steady heading averaged 2.2 µT vs 1.9 µT steady-state control —
  statistically indistinguishable, worth ≲6° worst-case. The earlier
  conclusion ("field changes while driving, static cal can't fix")
  mis-attributed an orientation-dependent swing to throttle. At rest
  the heading was stable within ±5–9°.
- **Why the cal was bad**: `magCalTick` gated plateau/min-range on chip
  X and Y — but chip X is VERTICAL (mount: X=up, Y=port, Z=stern). The
  procedure demanded ≥20 µT of range on an axis a flat spin can't move,
  and never validated chip Z, one of the two axes heading actually
  uses. The cal that "passed" likely involved hand-tilting, which
  contaminates min/max centers with the ~46 µT vertical field.

## `test_29-pool2.6-magcal2` 2026-06-12 — cal rework + true heading (UNTESTED)

Firmware changes:

1. **Cal coverage rework.** Min/max collection stays, but the finish
   condition is now rotation coverage: the horizontal field vector
   (chip Y/Z) is binned into 12 × 30° sectors around the running circle
   center; the cal completes when all 12 are visited and both
   horizontal ranges exceed 20 µT. Progress = sectors covered, so the
   app shows actual rotation, not a countdown. Plateau detection and
   the vertical-axis gate are gone. Extras: ±5 µT sample-to-sample
   spike filter; coverage re-bins if the center estimate drifts > 3 µT;
   timeout raised to 90 s with specific fail reasons ("weak signal" vs
   "incomplete rotation: N/12").
2. **Cal quality verdict.** On finish the boat grades its own spin:
   field-circle radius vs the expected local horizontal field
   (~20.5 µT) and Y-vs-Z radius mismatch (soft iron). good / fair /
   poor, persisted to NVS, reported in telemetry
   (`mag_cal_quality`, `mag_cal_radius_uT`, `mag_cal_circ_pct`).
   `mag_baseline_uT` is now the circle radius — the level-water |B|
   the live `mag_uT` should sit near at every heading, which makes the
   app's existing |B|-deviation warning meaningful.
3. **Declination.** `MAG_DECLINATION_DEG = -11.0` (Chesapeake). The
   `heading` field and AUTO steering now use TRUE heading; raw magnetic
   stays visible as `heading_mag`.
4. **GPS-COG trim.** 1 Hz servo loop nudges a `cog_trim` correction
   (±30° clamp, ~20 s time constant) toward GPS course when: fix valid,
   speed ≥ 1 kt, |turn rate| < 10°/s, forward thrust. Absorbs whatever
   cal residual remains. Resets on boot and on recal; at rest it just
   holds. Telemetry: `cog_trim`.

App changes (one commit): CalibrationScreen rebuilt — 12-dot coverage
ring driven by `mag_cal_mask` (dots only light when the boat actually
rotates), post-cal results card with quality verdict + plain-language
interpretation + verify hint; TelemetryScreen shows COG trim when
nonzero; types updated.

**Flash note: the OLD (bad) cal is still in NVS and will load on first
boot of pool2.6 with quality "unknown". Re-run the calibration before
trusting heading.** Heading will also read ~11° lower than pool2.5 on
the same boat orientation — that's the declination fix, not a
regression.

### Bench-the-boat gates (MC2-*, before next water run)

| Gate | What |
|------|------|
| MC2-1 | Flash `test_29-pool2.6-magcal2`. App Firmware row reads it. Manual drive, AUTO engage, bilge, depth, radar, audio smoke-test all unchanged. |
| MC2-2 | Start cal from the app, hold the boat STILL — coverage ring stays near 0/12 and does NOT advance (this is the anti-countdown proof). |
| MC2-3 | Rotate the boat slowly through a full circle — ring fills, cal self-finishes at 12/12, verdict + radius appear. Radius within ~30% of 20.5 µT ⇒ GOOD. |
| MC2-4 | Let it time out once (90 s, partial spin) — fail reason names the missing coverage ("N/12 directions"). |
| MC2-5 | Reboot. Cal + quality reload from NVS (`mag_from_nvs=true`, same verdict). |
| MC2-6 | After a GOOD cal: heading vs phone compass (set to TRUE north) at 4 cardinal bow orientations, ±10°. Boat still: two consecutive readings differ < 5°. |
| MC2-7 | Boat at rest, blip throttle to ~1700 µs — heading moves < 5° (motor-field sanity; analysis says ~2 µT ⇒ ≲6°). |
| MC2-8 | (Water) Straight leg > 30 s at > 1 kt: `cog_trim` converges and stays small (within ±10° after a GOOD cal); `heading` ≈ `course` while underway. |
| MC2-9 | Abort during collecting → state idle, NVS untouched. |

MC2-6 supersedes MC-7/MC-8. The MC-5 "`mag_baseline_uT > 20`" gate is
obsolete — baseline is now the horizontal radius, expected ≈ 20.5.

### Telemetry buffer + nav-light WiFi signal (review fixes, UNTESTED)

Two low-risk additions from the test_29 code review; neither changes how
the boat drives.

1. **Telemetry buffer.** The mag-cal/COG fields roughly doubled the
   field count, leaving the 1536-byte payload buffer near its limit — a
   maxed-out message could truncate into invalid JSON and blank the app
   (the only water-side telemetry surface). Bumped the JSON document and
   output buffer to 2048, and added a serial WARN that fires only if a
   payload ever exceeds 1800 bytes (`measureJson`). Quiet in normal use;
   gives a real number to drive future field-trimming.
2. **Nav-light WiFi signal.** `blinkNav()` pulses the nav LED at boot so
   WiFi state is readable across the pool: 1 flash when scanning starts,
   2 flashes once connected. Setup-only, LED off afterward.

| Gate | What |
|------|------|
| TB-1 | Flash, watch nav LED at boot: 1 blink as it starts scanning, 2 blinks when WiFi connects. LED off after. |
| TB-2 | App telemetry still parses and updates normally (no blank/stale screen). Serial shows NO `[telemetry] WARN` line during a full run with a waypoint set + cal idle. |
| TB-3 | (Optional) Trigger a cal while a waypoint is set, confirm app still updates — this is the heaviest payload case. |
