// websocketService.ts
// Telemetry via HTTP polling — same exported API as a WebSocket service so
// all screens and hooks work unchanged.
// Polls GET /telemetry roughly every 1 s. Self-chains (schedules the next
// poll only after the current one settles) so a slow/lossy hotspot can never
// have two requests in flight competing for the same weak link. Marks
// disconnected only after MISS_LIMIT consecutive failures — a single dropped
// poll at range recovers silently instead of flapping the UI.
//
// No connection-pool workaround: a URLSession/CFNetwork test (both timeout and
// AbortController-style cancel) showed the stack already closes an aborted poll's
// TCP connection and opens a fresh one on the next poll — there is no surviving
// "zombie" pooled socket to escape, so Connection:close / cache-buster hacks are
// inert. We only CLASSIFY each failure (timeout vs network-error) and expose it
// via getLastFailureKind(), so the flight log can record, per gap, whether the
// app saw a hung socket or a refused one — pairs with the boat-side wifi_assoc
// on backfilled rows to localize which layer actually dropped.

import { HTTP_PORT } from '../constants';
import { TelemetryData } from '../types';

type Listener = (data: TelemetryData) => void;
export type FailureKind = 'timeout' | 'neterror' | null;
// One failed poll during an outage. k='timeout' = our 2s abort fired with no
// response; k='neterror' = the fetch stack rejected (refused/reset/no-route —
// RN doesn't expose which). t = Date.now() when it resolved.
export interface PollAttempt { t: number; k: 'timeout' | 'neterror'; }

const POLL_MS    = 1000;
const TIMEOUT_MS = 2000;
const MISS_LIMIT = 2;   // consecutive failed polls before declaring disconnected

let timeoutId: ReturnType<typeof setTimeout> | null = null;
let currentIP  = '';
let misses     = 0;
let _lastFailureKind: FailureKind = null;
// Per-attempt outcome trace for the CURRENT failure streak — drained by the
// flight logger onto the reconnect frame so the next diagnostic run captures the
// app's view of a gap (were the failed polls timeouts = boat/AP unreachable, or
// neterrors = phone-side stack failure) alongside the boat's wifi_assoc. Reset
// at the start of each streak; capped so a very long gap can't grow it unbounded.
const TRACE_MAX = 600;
let failureTrace: PollAttempt[] = [];
let lastPollOk   = true;
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
  const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
  try {
    const res = await fetch(`http://${ip}:${HTTP_PORT}/telemetry`, {
      signal: controller.signal,
    });
    if (res.ok) {
      const data: TelemetryData = await res.json();
      misses     = 0;
      lastPollOk = true;   // leave failureTrace intact for the logger to drain
      _lastData  = data;
      _connected = true;
      listeners.forEach((fn) => fn(data));
    } else {
      // A real HTTP response (even non-2xx) means the boat answered — reachable,
      // not a hung socket. Count it as a network miss, not a hang.
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
  if (lastPollOk) failureTrace = [];   // first failure after a good poll → new streak
  lastPollOk = false;
  if (failureTrace.length < TRACE_MAX) failureTrace.push({ t: Date.now(), k: kind });
  if (misses >= MISS_LIMIT) _connected = false;
}

export function connect(ip: string) {
  if (timeoutId) { clearTimeout(timeoutId); timeoutId = null; }
  currentIP  = ip;
  _connected = false;
  _lastData  = null;
  misses     = 0;
  _lastFailureKind = null;
  failureTrace = [];
  lastPollOk   = true;
  poll();                                          // immediate first poll; chains itself
}

export function disconnect() {
  currentIP  = '';
  _connected = false;
  _lastData  = null;
  misses     = 0;
  _lastFailureKind = null;
  failureTrace = [];
  lastPollOk   = true;
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

// Drains (returns and clears) the per-attempt failure trace for the streak that
// just ended. The flight logger calls this on the reconnect frame to stamp the
// gap. Empty if the app wasn't polling (e.g. backgrounded) or there was no gap.
export function drainFailureTrace(): PollAttempt[] {
  const tr = failureTrace;
  failureTrace = [];
  return tr;
}

export function getLastData(): TelemetryData | null {
  return _lastData;
}

// IP currently being polled (''. if disconnected). Used by the telemetry
// logger to pull /history for gap backfill on reconnect.
export function getCurrentIP(): string {
  return currentIP;
}
