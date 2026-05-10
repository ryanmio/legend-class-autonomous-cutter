// Types for Legend Cutter app — mirror the JSON fields broadcast by the
// firmware's /telemetry endpoint. Source of truth as of test_29 (pool
// integration sketch). Most fields are optional because earlier test
// sketches send only a subset; screens must use optional chaining.

export interface TelemetryData {
  v: string;              // firmware/sketch version — always present
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
  esc_us?: number;

  // ── IMU ────────────────────────────────────────────────────────
  heading?: string;
  roll?: string;
  pitch?: string;
  accel_mag?: number;

  // ── Battery (INA219) ──────────────────────────────────────────
  batt_v?: string;        // bus voltage, V (string formatted)
  batt_a?: string;        // current draw, A
  batt_low?: boolean;     // production firmware will set; computed
                          //   client-side as fallback when absent
  batt_crit?: boolean;

  // ── GPS / position ────────────────────────────────────────────
  gps_fix?: boolean;
  gps_simulated?: boolean; // bench /sim_gps was used this session
  lat?: string;
  lon?: string;
  sats?: number;
  speed_kts?: string;
  course?: string;

  // ── Waypoint / autopilot ──────────────────────────────────────
  wp_set?: boolean;
  captured?: boolean;     // sticky once wp_dist < CAPTURE_RADIUS_M
  wp_lat?: string;
  wp_lon?: string;
  wp_dist_m?: string;     // string in test_29; some legacy code may
                          //   read as number — see telemetryFormat.ts
  wp_bearing?: string;
  heading_target?: string; // legacy from test_25; superseded by wp_*
  heading_error?: string;

  // ── PID (live values) ─────────────────────────────────────────
  pid_kp?: string;
  pid_kd?: string;

  // ── Aspirational fields (production firmware — currently absent) ─
  // Left optional so screens that reference them don't break when
  // wired against test_29; will populate when production firmware lands.
  bilge_fwd?: boolean;
  bilge_aft?: boolean;
  pump?: boolean;
  depth_m?: string;
  sonar_ok?: boolean;
  nav_on?: boolean;
  bridge_on?: boolean;
  deck_on?: boolean;
}

export interface Waypoint {
  lat: number;
  lon: number;
  label?: string;
}

export interface PIDParams {
  kp: number;
  kd: number;
  ki?: number;            // not used in pool — pool tuning is P+D
}

export type ConnectionStatus = 'idle' | 'connecting' | 'connected' | 'failed';

export type AnimMode =
  | 'none'
  | 'patrol_scan'
  | 'track_target'
  | 'combat_demo'
  | 'random_alert'
  | 'lrad_hail';

// Battery thresholds (V) for 4S LiPo. Land at yellow, urgent at red.
// 14.4 V = 3.6 V/cell ("low — return now"); 13.6 V = 3.4 V/cell
// ("critical — land immediately"). Conservative vs config.h's 13.0/13.6.
export const BATT_LOW_V  = 14.4;
export const BATT_CRIT_V = 13.6;
