// websocketService.ts
// Telemetry via HTTP polling — same exported API as a WebSocket service so
// all screens and hooks work unchanged.
// Polls GET /telemetry roughly every 1 s. Self-chains (schedules the next
// poll only after the current one settles) so a slow/lossy hotspot can never
// have two requests in flight competing for the same weak link. Marks
// disconnected only after MISS_LIMIT consecutive failures — a single dropped
// poll at range recovers silently instead of flapping the UI.

import { HTTP_PORT } from '../constants';
import { TelemetryData } from '../types';

type Listener = (data: TelemetryData) => void;

const POLL_MS    = 1000;
const TIMEOUT_MS = 2000;
const MISS_LIMIT = 2;   // consecutive failed polls before declaring disconnected

let timeoutId: ReturnType<typeof setTimeout> | null = null;
let currentIP  = '';
let misses     = 0;
const listeners  = new Set<Listener>();
let _connected   = false;
// Most recent telemetry frame received this session. Exposed via
// getLastData() so newly-mounted screens can hydrate from cache instead
// of starting empty and waiting up to a full poll cycle for the first
// frame — eliminates "default view + NO FIX" flashes on screen remount.
let _lastData:   TelemetryData | null = null;

async function poll() {
  const ip = currentIP;
  if (!ip) return;
  try {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
    const res = await fetch(`http://${ip}:${HTTP_PORT}/telemetry`, {
      signal: controller.signal,
    });
    clearTimeout(timer);
    if (res.ok) {
      const data: TelemetryData = await res.json();
      misses     = 0;
      _lastData  = data;
      _connected = true;
      listeners.forEach((fn) => fn(data));
    } else {
      registerMiss();
    }
  } catch {
    registerMiss();
  } finally {
    // Only re-chain if nothing else (a new connect()/disconnect()) has
    // already taken over while this poll was in flight.
    if (currentIP === ip) timeoutId = setTimeout(poll, POLL_MS);
  }
}

function registerMiss() {
  misses++;
  if (misses >= MISS_LIMIT) _connected = false;
}

export function connect(ip: string) {
  if (timeoutId) { clearTimeout(timeoutId); timeoutId = null; }
  currentIP  = ip;
  _connected = false;
  _lastData  = null;
  misses     = 0;
  poll();                                          // immediate first poll; chains itself
}

export function disconnect() {
  currentIP  = '';
  _connected = false;
  _lastData  = null;
  misses     = 0;
  if (timeoutId) { clearTimeout(timeoutId); timeoutId = null; }
}

export function subscribe(fn: Listener): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function isConnected(): boolean {
  return _connected;
}

export function getLastData(): TelemetryData | null {
  return _lastData;
}

// IP currently being polled (''. if disconnected). Used by the telemetry
// logger to pull /history for gap backfill on reconnect.
export function getCurrentIP(): string {
  return currentIP;
}
