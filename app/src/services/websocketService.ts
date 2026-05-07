// websocketService.ts
// Telemetry via HTTP polling — same exported API as a WebSocket service so
// all screens and hooks work unchanged.
// Polls GET /telemetry every 1 s; marks connected/disconnected based on response.

import { HTTP_PORT } from '../constants';
import { TelemetryData } from '../types';

type Listener = (data: TelemetryData) => void;

const POLL_MS    = 1000;
const TIMEOUT_MS = 2000;

let intervalId: ReturnType<typeof setInterval> | null = null;
let currentIP  = '';
const listeners  = new Set<Listener>();
let _connected   = false;

async function poll() {
  if (!currentIP) return;
  try {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
    const res = await fetch(`http://${currentIP}:${HTTP_PORT}/telemetry`, {
      signal: controller.signal,
    });
    clearTimeout(timer);
    if (res.ok) {
      const data: TelemetryData = await res.json();
      _connected = true;
      listeners.forEach((fn) => fn(data));
    } else {
      _connected = false;
    }
  } catch {
    _connected = false;
  }
}

export function connect(ip: string) {
  currentIP  = ip;
  _connected = false;
  if (intervalId) clearInterval(intervalId);
  poll();                                          // immediate first poll
  intervalId = setInterval(poll, POLL_MS);
}

export function disconnect() {
  currentIP  = '';
  _connected = false;
  if (intervalId) { clearInterval(intervalId); intervalId = null; }
}

export function subscribe(fn: Listener): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function isConnected(): boolean {
  return _connected;
}
