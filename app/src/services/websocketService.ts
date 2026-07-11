// websocketService.ts
// Telemetry via HTTP polling — same exported API as a WebSocket service so
// all screens and hooks work unchanged.
// Polls GET /telemetry roughly every 1 s. Self-chains (schedules the next
// poll only after the current one settles) so a slow/lossy hotspot can never
// have two requests in flight competing for the same weak link. Marks
// disconnected only after MISS_LIMIT consecutive failures — a single dropped
// poll at range recovers silently instead of flapping the UI.
//
// Dead-socket recovery: a per-request AbortController tears down a hung poll,
// but on iOS/Android the fetch stack keeps a keep-alive connection pool and can
// hand the next poll a half-dead pooled connection — which the plain retry never
// escapes (matches the "stuck for minutes, then suddenly reconnects" symptom).
// So we classify each failure (timeout vs network-error) and, after a run of
// TIMEOUTS, force each subsequent poll onto a brand-new TCP connection until one
// succeeds. getLastFailureKind() also exposes the failure type so the flight log
// can record, per gap, whether the app saw a hung socket or a refused one.

import { HTTP_PORT } from '../constants';
import { TelemetryData } from '../types';

type Listener = (data: TelemetryData) => void;
export type FailureKind = 'timeout' | 'neterror' | null;

const POLL_MS    = 1000;
const TIMEOUT_MS = 2000;
const MISS_LIMIT = 2;   // consecutive failed polls before declaring disconnected
// A hung socket the per-request abort can't self-heal: after this many
// consecutive TIMEOUTS, stop trusting the connection pool and force a fresh TCP
// connection on each poll, with a shorter timeout so we cycle through faster.
const STALL_LIMIT      = 3;
const STALL_TIMEOUT_MS = 1000;

let timeoutId: ReturnType<typeof setTimeout> | null = null;
let currentIP  = '';
let misses     = 0;
let consecutiveTimeouts = 0;
let forceFreshConn      = false;    // true → force a new TCP connection per poll
let reqSeq              = 0;        // cache-buster: a distinct URL under stall
let _lastFailureKind: FailureKind = null;
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
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(),
                           forceFreshConn ? STALL_TIMEOUT_MS : TIMEOUT_MS);
  try {
    // Under a stall, defeat the fetch stack's pooled-connection reuse: a unique
    // URL plus Connection: close forces a fresh TCP connection instead of a
    // half-dead pooled one. Connection is a hint some stacks strip, so the
    // unique URL is the reliable lever; /telemetry ignores query args.
    const suffix  = forceFreshConn ? `?_r=${++reqSeq}` : '';
    const headers = forceFreshConn ? { Connection: 'close' } : undefined;
    const res = await fetch(`http://${ip}:${HTTP_PORT}/telemetry${suffix}`, {
      signal: controller.signal,
      headers,
    });
    if (res.ok) {
      const data: TelemetryData = await res.json();
      misses              = 0;
      consecutiveTimeouts = 0;
      forceFreshConn      = false;   // recovered — resume normal keep-alive polling
      _lastData           = data;
      _connected          = true;
      listeners.forEach((fn) => fn(data));
    } else {
      // A real HTTP response (even non-2xx) means the boat answered — reachable,
      // not a hung socket. Count it as a network miss, not a stall.
      registerMiss('neterror');
    }
  } catch (e: any) {
    // AbortError = our timeout fired with no response (hung socket); anything
    // else (TypeError "Network request failed") = refused/reset/unreachable.
    registerMiss(e?.name === 'AbortError' ? 'timeout' : 'neterror');
  } finally {
    clearTimeout(timer);
    // Only re-chain if nothing else (a new connect()/disconnect()) has
    // already taken over while this poll was in flight.
    if (currentIP === ip) timeoutId = setTimeout(poll, POLL_MS);
  }
}

function registerMiss(kind: Exclude<FailureKind, null>) {
  misses++;
  _lastFailureKind = kind;
  if (kind === 'timeout') {
    // Hung socket: escalate to forced re-establish once a few pile up in a row.
    if (++consecutiveTimeouts >= STALL_LIMIT) forceFreshConn = true;
  } else {
    consecutiveTimeouts = 0;   // refused/reset ≠ hung; the far stack is responding
  }
  if (misses >= MISS_LIMIT) _connected = false;
}

export function connect(ip: string) {
  if (timeoutId) { clearTimeout(timeoutId); timeoutId = null; }
  currentIP  = ip;
  _connected = false;
  _lastData  = null;
  misses     = 0;
  consecutiveTimeouts = 0;
  forceFreshConn      = false;
  _lastFailureKind    = null;
  poll();                                          // immediate first poll; chains itself
}

export function disconnect() {
  currentIP  = '';
  _connected = false;
  _lastData  = null;
  misses     = 0;
  consecutiveTimeouts = 0;
  forceFreshConn      = false;
  _lastFailureKind    = null;
  if (timeoutId) { clearTimeout(timeoutId); timeoutId = null; }
}

export function subscribe(fn: Listener): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function isConnected(): boolean {
  return _connected;
}

// Kind of the most recent poll failure ('timeout' = hung socket, 'neterror' =
// refused/reset/unreachable), or null before any failure. Sticky across a
// recovery so the flight logger can read it on the reconnect frame and attribute
// the gap that just closed. Pairs with the boat-side wifi_assoc on backfilled
// rows to localize which layer dropped.
export function getLastFailureKind(): FailureKind {
  return _lastFailureKind;
}

export function getLastData(): TelemetryData | null {
  return _lastData;
}

// IP currently being polled (''. if disconnected). Used by the telemetry
// logger to pull /history for gap backfill on reconnect.
export function getCurrentIP(): string {
  return currentIP;
}
