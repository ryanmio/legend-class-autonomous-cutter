// Types for Legend Cutter app — mirror the JSON fields broadcast by telemetry.cpp

export interface TelemetryData {
  v: string;
  uptime: number;
  heap: number;
  batt_v: string;
  batt_a: string;
  batt_low: boolean;
  batt_crit: boolean;
  bilge_fwd: boolean;
  bilge_aft: boolean;
  pump: boolean;
  gps_fix: boolean;
  lat: string;
  lon: string;
  speed_kts: string;
  course: string;
  sats: number;
  heading: string;
  roll: string;
  pitch: string;
  depth_m: string;
  sonar_ok: boolean;
  // Added in later phases
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
