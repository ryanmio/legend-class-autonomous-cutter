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
//                           /pid, /sim_gps
//   PRODUCTION FIRMWARE   : /led  (toggles nav/bridge/deck lights —
//                                  returns 404 on test_29, lights
//                                  controls show in HelmScreen anyway
//                                  for production firmware readiness)

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

// Bench-debug only: inject a fake GPS position. Firmware sets
// gps_simulated=true sticky for the session.
export async function setSimGps(ip: string, lat: number, lon: number) {
  return post(ip, '/sim_gps', { lat, lon });
}

// Toggle hull lights. Returns 404 on test_29 — production firmware will
// implement /led. HelmScreen catches errors silently until then.
export async function setLed(ip: string, light: 'nav' | 'bridge' | 'deck', state: boolean) {
  return post(ip, '/led', { light, state });
}
