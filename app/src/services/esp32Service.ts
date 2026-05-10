// esp32Service.ts
// HTTP command client for the Legend Cutter ESP32.
// All requests go to http://<ip>:<HTTP_PORT>/<endpoint>.
//
// Endpoint status as of test_29 (pool integration sketch):
//   IMPLEMENTED in test_29: /status, /telemetry, /cruise, /waypoint,
//                           /pid, /sim_gps
//   NOT IMPLEMENTED YET    : /mode, /estop, /rth, /set-home, /waypoints,
//                            /calibrate-imu, /audio, /led, /gun, /ciws,
//                            /anim, /track, /door, /anchor, /radar
//   Calls to the not-implemented endpoints will return 404 from the
//   firmware. Kept here so the existing Weapons/Systems screens still
//   compile; they're not load-bearing for pool day.

import { HTTP_PORT } from '../constants';
import { PIDParams, AnimMode } from '../types';

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

// ── IMPLEMENTED in test_29 ───────────────────────────────────────────────

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

// Bench-debug only: inject a fake GPS position. Firmware sets
// gps_simulated=true sticky for the session.
export async function setSimGps(ip: string, lat: number, lon: number) {
  return post(ip, '/sim_gps', { lat, lon });
}

// ── NOT IMPLEMENTED in test_29 — production firmware backlog ─────────────

export async function setMode(ip: string, mode: 'manual' | 'autonomous') {
  return post(ip, '/mode', { mode });
}
export async function emergencyStop(ip: string) {
  return post(ip, '/estop');
}
export async function releaseEStop(ip: string) {
  return post(ip, '/estop-release');
}
export async function setGunPosition(ip: string, pan: number, tilt: number) {
  return post(ip, '/gun', { pan, tilt });
}
export async function setCIWS(ip: string, pan: number, spin: boolean) {
  return post(ip, '/ciws', { pan, spin });
}
export async function setAnimMode(ip: string, mode: AnimMode) {
  return post(ip, '/anim', { mode });
}
export async function setTrackBearing(ip: string, bearing: number) {
  return post(ip, '/track', { bearing });
}
export async function setBayDoor(ip: string, side: 'port' | 'stbd', action: 'open' | 'close' | 'stop') {
  return post(ip, '/door', { side, action });
}
export async function setAnchor(ip: string, which: 'fwd' | 'aft', action: 'lower' | 'raise' | 'stop') {
  return post(ip, '/anchor', { which, action });
}
export async function setRadar(ip: string, on: boolean) {
  return post(ip, '/radar', { on });
}
export async function setLed(ip: string, light: 'nav' | 'bridge' | 'deck', state: boolean) {
  return post(ip, '/led', { light, state });
}
export async function playAudio(ip: string, track: number) {
  return post(ip, '/audio', { track });
}
export async function setWaypoints(ip: string, waypoints: { lat: number; lon: number }[]) {
  return post(ip, '/waypoints', { waypoints });
}
export async function triggerRTH(ip: string) {
  return post(ip, '/rth');
}
export async function setHome(ip: string) {
  return post(ip, '/set-home');
}
export async function triggerIMUCalibration(ip: string) {
  return post(ip, '/calibrate-imu');
}
