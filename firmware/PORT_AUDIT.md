# Port audit — finishing the test-sketch → `legend_cutter/` port

**What this is.** `legend_cutter/` is the modular production firmware. It has
been built up *incrementally* — as each `test_NN_*` capability passed, its
piece was ported across. This pass **finishes** that work: it brings every
capability of the PASS'd `test_29_pool_integration` build into `legend_cutter/`
faithfully, while **keeping** the capabilities already ported from other
passing tests that test_29 itself doesn't include (the deck-gun turret from
test_18).

So the target firmware = **test_29's capability set ∪ the already-ported
test-folder capabilities** (today: + test_18 deck-gun pan). It is *not* a
strict 1:1 mirror of test_29 — test_29 is the source of truth for the
capabilities it *has*, but it is not the full feature ceiling of the boat.

**Source of truth for test_29 behavior:** `test_29_pool_integration.ino` at
commit `7b0f7cdd` (single-file sketch; constants inline — no test_29
`config.h`). For each test_29 capability, where `legend_cutter/` disagreed,
test_29 won.

**Headline:** several test_29 capabilities had **not been ported into
`legend_cutter/` yet** — most significantly the entire mag-calibration / NVS /
true-heading (declination + COG-trim) subsystem and the `mag_*` /
`heading_mag` / `cog_trim` telemetry the app consumes. This pass ports them.
The modules that *were* already ported (ibus, motors, battery, bilge, sonar,
audio, radar, lights, the test_18 turret) were spot-checked and kept.

---

## Retained from other passing tests (NOT part of test_29)

| Capability | Source | Status |
|---|---|---|
| **Deck-gun pan turret** — CH5 knob → PCA ch8 positional servo (clamp + ±15 µs center deadband + reverse; holds 1500 µs on RC loss) | **test_18 phase-2 PASS, 2026-05-04** | **Kept.** `weapons.{h,cpp}` + `GUN_PAN_*`/`CH_GUN_PAN`/`IBUS_IDX_GUN_PAN` config + `ch_gun_pan`/`gun_pan_us` telemetry. Verified against `test_18_deck_gun_pan.ino`. |

## Dropped (bench-only artifact, operator-confirmed)

| Dropped | Why | App impact |
|---|---|---|
| `/sim_gps` route + `gps_simulated` key + GPS sim state | Bench-only debug injection, not a water capability; a sticky sim flag is dangerous in water (project memory). Not present in test_29. | App reads neither; never calls `/sim_gps`. None. |

---

## A. Config constants — `config.h` vs test_29 inline

`config.h` was stale/incomplete for the test_29 capability set. Reconciled
from the test_29 `.ino`.

### Corrected values (test_29 wins)
| Constant | was | now (test_29) |
|---|---|---|
| `FIRMWARE_VERSION` | `0.3.0-pool2` | `0.4.0` (per plan) |
| `MAX_FWD_US` | 1710 | **1800** |
| `MIN_REV_US` | 1290 | **1200** |
| `AUTO_CRUISE_CAP_US` | 1700 | **1800** |
| `DEFAULT_CRUISE_US` | 1620 | **1720** |

### Added (test_29 has, config.h lacked)
- `MAG_DECLINATION_DEG = -11.0`, `EXPECTED_HORIZ_FIELD_UT = 20.5`
- COG trim: `COG_TRIM_MIN_KTS`, `COG_TRIM_MAX_TURN_DPS`, `COG_TRIM_GAIN`, `COG_TRIM_CLAMP_DEG`, `COG_TRIM_INTERVAL_MS`
- Mag cal: `MAG_CAL_MIN_MS`, `MAG_CAL_TIMEOUT_MS`, `MAG_CAL_MIN_RANGE`, `MAG_CAL_SECTORS`, `MAG_CAL_SPIKE_UT`, `MAG_CAL_CENTER_SHIFT_UT`
- `MAX_WP_DIST_M = 1000.0` (fat-finger guard)

### Renamed (same value)
- `MAG_OFFSET_X/Y/Z` → `DEFAULT_MAG_OFFSET_X/Y/Z` (−20.70 / −0.45 / −17.70): now the NVS-empty fallback.
- `IMU_FILTER_ALPHA` keeps its name (= test_29 `ALPHA` = 0.98).

### Kept (test_18 turret)
- `IBUS_IDX_GUN_PAN`=4, `CH_GUN_PAN`=8, `GUN_PAN_MIN_US`=1000, `GUN_PAN_MAX_US`=2000, `GUN_PAN_DEADBAND_US`=15, `GUN_PAN_REVERSE`=true.

### Already matched
PCA ch 0/1/2; `RUDDER_MIN/MAX_US` 1330/1670; `NEUTRAL_US`; `ESC_DIRECTION_INVERTED`; `DIFF_THRUST_FACTOR`; iBUS idx 0/1/2/5/6 + RX 16; `REVERSE_DEADBAND_US`; `THROTTLE_IDLE_MAX`; mode hysteresis; failsafe; `IMU_UPDATE_INTERVAL_MS`; `DEFAULT_KP/KD`; GPS pins/baud; sonar; bilge; radar; LED; DFP; `CAPTURE_RADIUS_M`; addresses. (Benign infra centralizations of test_29-hardcoded values: `I2C_SDA/SCL_PIN`, `I2C_FREQ_HZ`, `PCA9685_ADDR`, `ICM20948_ADDR`, `IBUS_BAUD`, `PCA9685_FREQ_HZ`, `HTTP_PORT` — each verified against test_29 inline usage.)

---

## B. IMU module — `imu.cpp/.h` : the largest piece newly ported

The IMU module predated test_29's mag-cal / true-heading work, so it carried
only the complementary filter. Ported the rest:

| test_29 capability | before this pass |
|---|---|
| Complementary filter (axis remap `mr=-mz,-my,-mx`; yaw-rate fusion; ALPHA) | present, kept |
| `headingRateDps` computed in updateImu at IMU cadence; D-term uses it | not yet — D was recomputed at loop rate (the spiky-D path test_29's comment warns against). Now ported. |
| `imuHeadingTrue()` = fused + `MAG_DECLINATION_DEG` + `cogTrimDeg` | not yet — heading-hold steered *magnetic*. Now true. |
| `updateCogTrim()` 1 Hz GPS-COG residual learner | not yet — added |
| NVS-backed mag cal (`Preferences "imu_cal"`) | not yet — added |
| Mag-cal FSM (12-sector coverage, spike filter, quality, radius, circ_pct) | not yet — added |
| `liveMagUT`, `magCalibratedFlag()` | not yet — added |

`updateCogTrim` reads GPS (speed/course/valid) and the ESC output average
(`motorsPortUs()`+`motorsStbdUs()`), mirroring test_29's use of `gps.*` and
`outPort/outStbd`.

---

## C. Navigation — `navigation.cpp/.h` : behavioral pieces newly ported

| test_29 capability | before |
|---|---|
| Capture + leg-start run **only in AUTO** (`mode != MODE_AUTO → return`) | ran in any mode — now gated via `navUpdate(inAuto)` |
| `MAX_WP_DIST_M` reject in `/waypoint` when GPS valid | added (`navTrySetWaypoint`) |
| `MAX_WP_DIST_M` auto-clear backstop in geometry update | added |
| Leg-start re-recorded at AUTO-engage (`navResetLegStart()` from FSM) | added |
| haversine bearing/dist, dual-trigger capture, clear-on-new-WP | kept |

---

## D. GPS — `gps.cpp/.h`

Real-fix path + 5 s stale-clear kept (matches test_29 `updatePosition`). GPS
sim path removed (see "Dropped" above).

---

## E. Telemetry / comms — `telemetry.cpp` : the app contract

### Telemetry JSON keys
- **Added** (test_29 has, app consumes): `failsafe_ack`, `heading_mag`, `cog_trim`, and the full mag suite (`mag_cal_state`, `mag_cal_progress`, `mag_calibrated`, `mag_cal_ts`, `mag_from_nvs`, `mag_cal_mask`, `mag_cal_fail`, `mag_cal_quality`, `mag_cal_radius_uT`, `mag_cal_circ_pct`, `mag_off_x/y/z`, `mag_baseline_uT`, `mag_uT`).
- **Corrected:** `heading` is now `imuHeadingTrue()` (true); `heading_mag` carries the fused magnetic value.
- **Kept (turret):** `ch_gun_pan`, `gun_pan_us`.
- **Buffer:** 1536 → 2048 (test_29 size; the mag suite overflowed 1536). Added test_29's `measureJson` >1800 serial warn.
- **`v` value:** now `"0.4.0"` (was test_29 literal `"test_29-pool2.6-magcal2"`). Intended version bump; `v` is informational (app uses `session_id` for reboot detect).
- `sats` now emitted unconditionally, speed/course on `isValid()` (matches test_29; the previous `!gpsSimulated()` gate went away with sim).

### HTTP routes
- **Added:** `/calibrate_mag/start`, `/calibrate_mag/abort` (POST+OPTIONS).
- **Removed:** `/sim_gps` (see Dropped).
- Kept: `/status`, `/telemetry`, `/cruise`, `/waypoint`, `/pid`, `/led`, `/audio`, `/bilge`, `/radar`, `/depth`. `/waypoint` gained the `MAX_WP_DIST_M` reject.

### WiFi — newly ported from test_29
- **scan-first** connect (home exact → any SSID containing "iPhone" → blind hotspot), `blinkNav(1/2)` boot signal, and a non-blocking **`wifiMaintain()`** in-loop reconnect (≤1 attempt / 30 s). The previous blind home→hotspot connect (no scan, no reconnect) is replaced.

---

## F. Mode FSM + loop — `legend_cutter.ino`

- On →AUTO transition: `navResetLegStart()` (re-record leg start at engage) — added.
- `failsafeAckRequired` surfaced via `vesselFailsafeAck()` extern → `failsafe_ack` telemetry — added.
- Loop: `imuUpdateCogTrim()` each pass; `wifiMaintain()` folded into `telemetryUpdate()` — added.
- AUTO output (clamp cruise to cap, true-heading hold, diff thrust; else neutral) — kept; now correct via the IMU true-heading port.
- `weaponsBegin()`/`weaponsUpdate()` — kept (turret).

---

## G. Modules kept as-is (spot-checked line-by-line vs test_29 / test_18)

ibus, motors (esc mirror + clamps + interlock + diff mix), battery, bilge,
sonar, audio (DF1201S pause+50 ms), radar (LEDC burst FSM), lights, and the
test_18 turret. (radar `/radar` handler: clamp `speed` int before the uint8
cast, matching test_29.)

---

## Stage 1 results (post-port)

**Clean build:** `arduino-cli compile --fqbn esp32:esp32:esp32 firmware/legend_cutter`
→ OK. Flash ≈1,056 KB (80%); RAM 50 KB (15%). Only ArduinoJson v7
`StaticJsonDocument` deprecation notes (same as test_29).

**Mechanical post-port diff** (`/tmp/portdiff.py`, both directions):

- **Telemetry keys:** all 73 test_29 keys present, same relative order; the
  only extras are the **2 intended turret keys** (`ch_gun_pan`, `gun_pan_us`).
  The app reads neither, so it parses identically.
- **HTTP routes:** identical to test_29 (the turret is RC-knob-driven, no route).
- **Config constants:** all 84 test_29 `static const` values present and
  matching (incl. the `unsigned long` mag-cal constants and the
  `ALPHA`→`IMU_FILTER_ALPHA` rename). Extra constants are the test_18 turret
  set + benign infra centralizations, all value-verified.

So the contract delta vs test_29 is exactly the retained test_18 turret
surface — intentional — and nothing else.

**Open (operator-gated):** Stage 2 bench-parity flash + Stage 4 water run are
the operator's (the agent can't flash). The CLAUDE.md / memory cutover is held
until the operator confirms bench parity.
