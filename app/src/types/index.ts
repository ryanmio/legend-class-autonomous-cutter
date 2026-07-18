// Types for Legend Cutter app — mirror the JSON fields broadcast by the
// firmware's /telemetry endpoint. Source of truth as of test_29 (pool
// integration sketch). Most fields are optional because earlier test
// sketches send only a subset; screens must use optional chaining.

export interface TelemetryData {
  v: string;              // firmware/sketch version — always present
  session_id?: number;    // hardware-random per boot; app uses to detect mid-flight reboots
  uptime?: number;        // seconds since boot
  heap?: number;          // free heap bytes

  // ── Mode + RC ───────────────────────────────────────────────────
  mode?: 'MANUAL' | 'AUTO' | 'FAILSAFE' | 'IDLE' | 'ESTOP';
  cruise_us?: number;     // 1500..1800 — current AUTO cruise µs
  failsafe_ack?: boolean; // sticky ack required (failsafe latched)
  rc_ever_good?: boolean;
  rc_age_ms?: number;
  ch_throttle?: number;
  ch_rudder?: number;
  ch_mode?: number;
  ch_guard?: number;

  // ── Outputs ────────────────────────────────────────────────────
  rudder_us?: number;
  port_us?: number;
  stbd_us?: number;

  // ── IMU ────────────────────────────────────────────────────────
  heading?: string;       // best TRUE heading (mag + declination + COG trim)
  heading_mag?: string;   // raw fused magnetic heading, pre-correction
  cog_trim?: string;      // GPS-COG residual trim currently applied, deg
  roll?: string;
  pitch?: string;
  accel_mag?: number;

  // ── Mag calibration / health (added test_29-pool2.1-magcal) ──
  mag_cal_state?: 'idle' | 'collecting' | 'done' | 'failed';
  mag_cal_progress?: number;       // 0..100, % of rotation sectors covered
  mag_cal_mask?: number;           // 12-bit sector coverage bitmap (collecting only)
  mag_calibrated?: boolean;        // true if a real cal exists in NVS
  mag_from_nvs?: boolean;          // true if offsets came from NVS (not hardcoded fallback)
  mag_cal_ts?: number;             // uptime-seconds when cal was last saved
  mag_cal_fail?: string;           // present only when state=='failed'
  mag_cal_quality?: 'unknown' | 'good' | 'fair' | 'poor';
  mag_cal_radius_uT?: string;      // horizontal field-circle radius from the spin
  mag_cal_circ_pct?: string;       // Y-vs-Z radius mismatch %, soft-iron indicator
  mag_off_x?: string;              // current applied offsets (post-cal or defaults)
  mag_off_y?: string;
  mag_off_z?: string;
  mag_baseline_uT?: string;        // expected level-water |B| post-cal; 0 = no cal
  mag_uT?: string;                 // live magnitude post-offset

  // ── Battery (INA219) ──────────────────────────────────────────
  batt_v?: string;        // bus voltage, V (string formatted)
  batt_a?: string;        // current draw, A
  batt_low?: boolean;     // production firmware will set; computed
                          //   client-side as fallback when absent
  batt_crit?: boolean;

  // ── GPS / position ────────────────────────────────────────────
  gps_fix?: boolean;
  lat?: string;
  lon?: string;
  sats?: number;
  speed_kts?: string;
  course?: string;

  // ── Waypoint / autopilot ──────────────────────────────────────
  // Mission (v0.7.0+): wp_* report the ACTIVE leg; the fields below give
  // sequencer state. A single waypoint is a 1-point mission.
  mission_active?: boolean; // a mission is loaded and still has a leg to drive
  wp_count?: number;        // total waypoints in the mission
  wp_idx?: number;          // 0-based active leg index
  wp_set?: boolean;
  captured?: boolean;     // mission complete (all legs captured); 1-pt → single-WP capture
  captured_by?: 'none' | 'distance' | 'crossing';  // which trigger fired
  wp_lat?: string;
  wp_lon?: string;
  wp_dist_m?: string;     // string in test_29; some legacy code may
                          //   read as number — see telemetryFormat.ts
  wp_bearing?: string;
  // Leg start = boat position recorded on first GPS fix after /waypoint
  // was POSTed. App can draw the leg line + perpendicular capture line.
  wp_start_lat?: string;
  wp_start_lon?: string;
  heading_target?: string; // legacy from test_25; superseded by wp_*
  heading_error?: string;

  // ── PID (live values) ─────────────────────────────────────────
  pid_kp?: string;
  pid_kd?: string;
  diff_gain?: string;      // AUTO diff-thrust gain (v0.8.0+)

  // Bilge / damage-control. Three zones (fwd, mid, rear). Pump moved to
  // the REAR compartment 2026-05-27 — only `bilge_rear` drives auto-pump;
  // fwd/mid are informational. The pump pulses 6 s on / 6 s off in bursts:
  //   AUTO runs BILGE_BURST_CYCLES (3) pulses, then a `cooldown` rest, and
  //   repeats forever (v0.11.0 — never gives up; the old 60 s cap + pump_stuck
  //   were removed). pump_phase reports the current phase, pump_cycle the
  //   1-based ON-pulse number within the current burst, pump_phase_ms the time
  //   elapsed in the current phase. Manual cycles forever until stopped via
  //   POST /bilge {on:false}.
  bilge_fwd?: boolean;
  bilge_mid?: boolean;
  bilge_rear?: boolean;
  bilge_aft?: boolean;        // legacy alias (pre-3-zone telemetry); unused going forward
  pump?: boolean;             // MOSFET state (HIGH during the ON phase only)
  pump_manual?: boolean;
  pump_stuck?: boolean;       // deprecated (v0.11.0): auto no longer gives up — never sent by firmware
  pump_phase?: 'off' | 'on' | 'pause' | 'cooldown';
  pump_cycle?: number;        // 1-based ON-pulse count in current burst; absent when off
  pump_phase_ms?: number;     // ms since current phase began; absent when off

  // Depth sonar (RCWL-1655). Present from test_29 onward.
  depth_m?: string;             // metres, 2-decimal string. Absent = no valid echo (stop, post-boot, or no clean bottom return).
  depth_mode?: 'off' | 'run';
  depth_age_ms?: number;        // millis since last ping attempt (present once the sonar has pinged, even with no valid echo)
  depth_raw_us?: number;        // raw echo pulse width µs (v0.6.1+). 0 = timeout/no return; >0 below the ~1 m floor = near-field artifact

  sonar_ok?: boolean;           // aspirational — not in test_29

  nav_on?: boolean;
  bridge_on?: boolean;
  deck_on?: boolean;
  audio_ok?: boolean;
  radar_on?: boolean;
  radar_speed?: number;     // 0-100, current PWM duty during burst
  radar_burst_ms?: number;  // burst ON phase length
  radar_pause_ms?: number;  // burst OFF phase length
}

export interface PIDParams {
  kp: number;
  kd: number;
  ki?: number;            // not used in pool — pool tuning is P+D
  diff_gain?: number;     // AUTO diff-thrust gain [0..2] (v0.8.0+)
}

export type ConnectionStatus = 'idle' | 'connecting' | 'connected' | 'failed';

// Battery thresholds (V) for 4S LiPo. Land at yellow, urgent at red.
// 14.4 V = 3.6 V/cell ("low — return now"); 13.6 V = 3.4 V/cell
// ("critical — land immediately"). Conservative vs config.h's 13.0/13.6.
export const BATT_LOW_V  = 14.4;
export const BATT_CRIT_V = 13.6;
