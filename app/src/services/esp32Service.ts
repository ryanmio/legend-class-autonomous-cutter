// esp32Service.ts
// HTTP command client for the Legend Cutter ESP32.
// All requests go to http://<ip>:<HTTP_PORT>/<endpoint>.

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

// ---- Connection ----
export async function checkStatus(ip: string) {
  const res = await fetchWithTimeout(url(ip, '/status'));
  if (!res.ok) throw new Error(`Status check failed: ${res.status}`);
  return res.json();
}

// ---- Sailing controls ----
export async function setMode(ip: string, mode: 'manual' | 'autonomous') {
  return post(ip, '/mode', { mode });
}

export async function emergencyStop(ip: string) {
  return post(ip, '/estop');
}

export async function releaseEStop(ip: string) {
  return post(ip, '/estop-release');
}

// ---- Weapons ----
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

// ---- Systems ----
export async function setBayDoor(ip: string, side: 'port' | 'stbd', action: 'open' | 'close' | 'stop') {
  return post(ip, '/door', { side, action });
}

export async function setAnchor(ip: string, which: 'fwd' | 'aft', action: 'lower' | 'raise' | 'stop') {
  return post(ip, '/anchor', { which, action });
}

export async function setRadar(ip: string, on: boolean) {
  return post(ip, '/radar', { on });
}

export async function playAudio(ip: string, track: number) {
  return post(ip, '/audio', { track });
}

// ---- Navigation ----
export async function setWaypoints(ip: string, waypoints: { lat: number; lon: number }[]) {
  return post(ip, '/waypoints', { waypoints });
}

export async function triggerRTH(ip: string) {
  return post(ip, '/rth');
}

export async function setHome(ip: string) {
  return post(ip, '/set-home');
}

// ---- Settings ----
export async function setPID(ip: string, params: PIDParams) {
  return post(ip, '/pid', params);
}

export async function triggerIMUCalibration(ip: string) {
  return post(ip, '/calibrate-imu');
}
