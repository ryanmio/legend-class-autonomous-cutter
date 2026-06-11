// esp32Service.ts
// HTTP command client for the Legend Cutter ESP32.
// All requests go to http://<ip>:<HTTP_PORT>/<endpoint>.
//
// Only endpoints the app actually calls live here. Helpers for future
// firmware features (E-STOP, RTH, multi-waypoint missions, deck gun,
// audio, IMU calibration, radar, mode override, etc.) will be added back
// when the firmware exposes them — keeping them as stubs that 404'd was
// noise that pretended the app did more than it does.
//
// Endpoint status as of test_29 (pool integration sketch):
//   IMPLEMENTED in test_29: /status, /telemetry, /cruise, /waypoint,
//                           /pid, /led, /audio, /bilge,
//                           /radar, /depth

import { HTTP_PORT } from '../constants';
import { PIDParams } from '../types';

const TIMEOUT_MS = 4000;

async function fetchWithTimeout(url: string, options: RequestInit = {}): Promise<Response> {
  const controller = new AbortController();
  const id = setTimeout(() => controller.abort(), TIMEOUT_MS);
  try {
    return await fetch(url, { ...options, signal: controller.signal });
  } finally {
    clearTimeout(id);
  }
}

function url(ip: string, path: string) {
  return `http://${ip}:${HTTP_PORT}${path}`;
}

async function post(ip: string, path: string, body?: object) {
  const res = await fetchWithTimeout(url(ip, path), {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : undefined,
  });
  if (!res.ok) throw new Error(`${path} failed: ${res.status}`);
  return res.json();
}

export async function checkStatus(ip: string) {
  const res = await fetchWithTimeout(url(ip, '/status'));
  if (!res.ok) throw new Error(`Status check failed: ${res.status}`);
  return res.json();
}

// AUTO cruise µs. Either {us: 1500..1800} or {pct: 0..100}.
// us=1500 (NEUTRAL) is valid — used for static heading-hold.
export async function setCruise(ip: string, args: { us?: number; pct?: number }) {
  return post(ip, '/cruise', args);
}

// Single active waypoint for AUTO. Pass lat=null, lon=null to clear.
// New /waypoint POST resets the captured latch in firmware.
export async function setWaypoint(ip: string, lat: number | null, lon: number | null) {
  return post(ip, '/waypoint', { lat, lon });
}

// Live PID tuning. Either or both fields accepted; firmware ignores ki.
export async function setPID(ip: string, params: PIDParams) {
  return post(ip, '/pid', params);
}

// Toggle hull lights.
export async function setLed(ip: string, light: 'nav' | 'bridge' | 'deck', state: boolean) {
  return post(ip, '/led', { light, state });
}

// Manual bilge-pump override. on=true forces the pump on until on=false
// or 60 s elapses (firmware auto-clears to prevent forgotten "on" from
// draining battery). Auto-pump-on-leak still fires regardless.
export async function postBilge(ip: string, on: boolean) {
  return post(ip, '/bilge', { on });
}

// Depth sonar (RCWL-1655). 'run' = ping every 20 s, 'check' = single
// ping, 'stop' = halt + clear last reading. Last reading persists in
// telemetry across mode changes EXCEPT stop, which clears it.
export async function setDepth(ip: string, mode: 'stop' | 'check' | 'run') {
  return post(ip, '/depth', { mode });
}

// Radar mast motor (TRS-3D dish). Burst-only — short PWM bursts with
// pauses between to fake slow rotation from a too-fast geared motor.
// `speed` is PWM duty during the burst (0-100%). `burst_ms` /
// `pause_ms` are live-tunable for iteration without reflashing.
// `on:false` cuts output.
export async function setRadar(ip: string, args: {
  on?: boolean;
  speed?: number;
  burst_ms?: number;
  pause_ms?: number;
}) {
  return post(ip, '/radar', args);
}

// Mag calibration: enter onboard plateau-detect cal mode. Operator
// rotates the whole boat through 360° on a flat surface; firmware
// computes hard-iron offsets, writes to NVS, applies to live heading.
// Idempotent — if already collecting, returns current state.
export async function postCalibrateMagStart(ip: string) {
  return post(ip, '/calibrate_mag/start');
}

// Exit cal mode without saving — NVS untouched.
export async function postCalibrateMagAbort(ip: string) {
  return post(ip, '/calibrate_mag/abort');
}

// Play an audio cue on the DF1201S. test_29 ignores `sound` and plays
// track 1 for any value; the key still goes over the wire so firmware
// can branch on it once track mapping is decided.
export async function playAudio(ip: string, sound: 'horn' | 'board' | 'gun') {
  console.log(`[audio] POST /audio sound=${sound}`);
  try {
    const res = await post(ip, '/audio', { sound });
    console.log(`[audio] OK`, res);
    return res;
  } catch (e: any) {
    console.warn(`[audio] FAIL sound=${sound}:`, e?.message ?? e);
    throw e;
  }
}
