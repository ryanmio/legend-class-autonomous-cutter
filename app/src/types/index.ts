// Types for Legend Cutter app — mirror the JSON fields broadcast by telemetry.cpp.
// Most fields are optional because test sketches send only a subset.
// All screens must use optional chaining / nullish coalescing on these fields.

export interface TelemetryData {
  v: string;              // firmware/sketch version — always present
  uptime?: number;        // seconds since boot
  heap?: number;          // free heap bytes
  batt_v?: string;
  batt_a?: string;
  batt_low?: boolean;
  batt_crit?: boolean;
  bilge_fwd?: boolean;
  bilge_aft?: boolean;
  pump?: boolean;
  gps_fix?: boolean;
  lat?: string;
  lon?: string;
  speed_kts?: string;
  course?: string;
  sats?: number;
  heading?: string;
  roll?: string;
  pitch?: string;
  depth_m?: string;
  sonar_ok?: boolean;
  nav_on?: boolean;
  bridge_on?: boolean;
  deck_on?: boolean;
  mode?: 'MANUAL' | 'AUTONOMOUS' | 'FAILSAFE' | 'ESTOP' | 'IDLE';
}

export interface Waypoint {
  lat: number;
  lon: number;
  label?: string;
}

export interface PIDParams {
  kp: number;
  ki: number;
  kd: number;
}

export type ConnectionStatus = 'idle' | 'connecting' | 'connected' | 'failed';

export type AnimMode =
  | 'none'
  | 'patrol_scan'
  | 'track_target'
  | 'combat_demo'
  | 'random_alert'
  | 'lrad_hail';
