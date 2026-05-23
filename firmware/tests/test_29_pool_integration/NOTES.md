# test_29_pool_integration — Notes

## Status: BENCH-VERIFIED 2026-05-16 (GPS + audio + LEDs all working); pool run pending

## What this sketch is

Production-direction sketch for the **first water test**. Not a bench
gate test — there are no auto-detected gates. The operator drives via
the app; the sketch operates.

Merges:
- **test_27** Mode FSM, CH6 SwD failsafe guard with sticky ACK, /cruise,
  /telemetry, INA219 voltage telemetry.
- **test_25** Single /waypoint + haversine bearing + heading-hold.
- **NEW** Capture detection (sticky), live /pid tuning, /sim_gps for
  bench dry-runs of the app.

## What's NEW vs everything before

1. **POST /waypoint** — `{lat, lon}` arms a single waypoint; `{lat:null,
   lon:null}` clears. Already wired in `app/src/screens/MapScreen.tsx`.
2. **Capture detection** — sticky `captured` flag fires on EITHER:
   - **DISTANCE:** `wp_dist < 3 m` (the existing trigger).
   - **CROSSING:** boat passes the perpendicular line through the
     waypoint, perpendicular to the leg from start→waypoint. Start
     point = boat position on the first GPS fix after `/waypoint` was
     POSTed. Stops the boat circling forever when GPS noise (~2-3 m)
     is close to the capture radius (3 m). Whichever fires first wins.
   ESCs + rudder forced neutral on capture. Serial logs which trigger
   fired (`captured by DISTANCE` vs `captured by CROSSING`).
3. **POST /pid {kp, kd}** — live heading-hold tuning during the run.
   Defaults Kp=3.0, Kd=8.0 (test_27/test_28 baseline). Ki=0 — pool
   tuning is P+D only.
4. **POST /sim_gps {lat, lon}** — bench-debug position injection.
   Sticky for the session. Don't POST in water.
5. **Cruise floor refusal removed** — cruise=NEUTRAL_US (1500) is now
   valid. Static-heading-hold scenario from AUTOPILOT_PLAN test_32
   step 2 just works.
6. **AUTO without a waypoint = neutral** — test_27's "AUTO holds
   heading at entry, runs cruise" placeholder is replaced. From here,
   AUTO means "drive to the active waypoint" or stay neutral.
7. **POST /led + /audio** — HelmScreen light toggles (nav / bridge /
   deck on GPIO 18 / 19 / 23) and the three sound buttons. Audio
   plays by **path** via `playSpecFile()` (AT+PLAYFILE under the
   hood) — order-independent, so the DF1201S USB mass-storage can be
   loaded with a plain Finder drop. Mapping: horn →
   `/cutter-horn.mp3`, gun → `/deck-gun.mp3`, board → internal-flash
   track 1 (placeholder; no boarding-party clip yet). Repo
   source-of-truth: `audio-assets/dfplayer/`. Telemetry exposes
   `nav_on`, `bridge_on`, `deck_on`, `audio_ok` so the app can
   reconcile its optimistic state and surface "audio dead" without
   USB.

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
| POST   | /waypoint   | `{lat, lon}` or `{lat:null,lon:null}` | arms / clears waypoint. On set: records `captured=false` and clears the leg-start latch; firmware re-records the leg-start position on the next valid GPS fix. |
| POST   | /pid        | `{kp, kd}` (either or both)    | live tuning |
| POST   | /sim_gps    | `{lat, lon}`                   | bench-only position injection |
| POST   | /led        | `{light:"nav"\|"bridge"\|"deck", state:bool}` | toggles nav (GPIO18) / bridge (GPIO19) / deck (GPIO23) |
| POST   | /audio      | `{sound:"horn"\|"board"\|"gun"}` | horn → `/cutter-horn.mp3`, gun → `/deck-gun.mp3` (path-based via `playSpecFile`); board → internal-flash track 1 (placeholder, no clip yet). 400 on unknown sound, 503 if DF1201S didn't ACK at boot. |
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
  "gps_simulated":bool,
  "lat":          "X.XXXXXX" (if gps_fix),
  "lon":          "X.XXXXXX" (if gps_fix),
  "sats":         int       (real GPS only),
  "speed_kts":    "X.X"     (real GPS only),
  "course":       "X.X"     (real GPS only),
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
