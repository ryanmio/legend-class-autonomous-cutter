// websocketService.ts
// Telemetry via HTTP polling — same exported API as a WebSocket service so all
// screens and hooks work unchanged. Polls GET /telemetry ~every 1 s, self-chaining
// (schedules the next poll only after the current settles) so a lossy hotspot never
// has two requests competing for the weak link. Marks disconnected only after
// MISS_LIMIT consecutive failures so one dropped poll doesn't flap the UI.
//
// Each failed poll is classified — timeout (our abort fired, no response) vs
// neterror (the fetch stack rejected: refused/reset/unreachable) — and tallied for
// the current failure streak. drainFailCounts() lets the flight logger stamp, per
// gap, how many of each the app saw: the app-side half of the reconnect-layer
// question, paired with the boat's wifi_assoc on the backfilled rows.

import { HTTP_PORT } from '../constants';
import { TelemetryData } from '../types';

type Listener = (data: TelemetryData) => void;
type FailureKind = 'timeout' | 'neterror';

const POLL_MS    = 1000;
const TIMEOUT_MS = 2000;
const MISS_LIMIT = 2;   // consecutive failed polls before declaring disconnected

let timeoutId: ReturnType<typeof setTimeout> | null = null;
let currentIP  = '';
let misses     = 0;
// Failed-poll tally since the last successful poll (i.e. the current gap). Reset
// after each success once the logger has had its chance to read it.
let failTimeouts  = 0;
let failNeterrors = 0;
const listeners  = new Set<Listener>();
let _connected   = false;
// Most recent telemetry frame received this session, exposed via getLastData() so
// a newly-mounted screen hydrates from cache instead of flashing empty state.
let _lastData:   TelemetryData | null = null;

async function poll() {
  const ip = currentIP;
  if (!ip) return;
  const startedAt = Date.now();
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
  let fresh: TelemetryData | null = null;
  try {
    const res = await fetch(`http://${ip}:${HTTP_PORT}/telemetry`, { signal: controller.signal });
    if (res.ok) {
      fresh      = await res.json();
      misses     = 0;
      _lastData  = fresh;
      _connected = true;
    } else {
      // A real HTTP response (even non-2xx) means the boat answered — reachable,
      // not a hung socket. Count it as a network miss.
      registerMiss('neterror');
    }
  } catch (e: any) {
    // AbortError = our timeout fired with no response (hung socket); anything
    // else (TypeError "Network request failed") = refused/reset/unreachable.
    registerMiss(e?.name === 'AbortError' ? 'timeout' : 'neterror');
  } finally {
    clearTimeout(timer);
    // Re-chain on a fixed wall-clock cadence: subtract the time this poll already
    // took (RTT + parse) so successive polls start ~POLL_MS apart, not
    // POLL_MS + RTT apart. A fixed period avoids aliasing the boat's 1 Hz
    // telemetry, which previously dropped ~1 boat-second every ~13 s. Clamp ≥0 so
    // a poll slower than POLL_MS just fires the next immediately.
    // Only re-chain if nothing else (a new connect()/disconnect()) took over.
    if (currentIP === ip) {
      timeoutId = setTimeout(poll, Math.max(0, POLL_MS - (Date.now() - startedAt)));
    }
  }
  if (fresh) {
    const data = fresh;
    // Dispatch outside the try so a throwing subscriber can't be misread as a
    // wire failure; isolate each so one bad listener can't starve the rest.
    listeners.forEach((fn) => { try { fn(data); } catch { /* a subscriber bug must not fail the poll */ } });
    // The failure streak that preceded this frame is now closed (the logger read
    // its counts above if this was a reconnect), so start the next gap clean.
    failTimeouts = 0;
    failNeterrors = 0;
  }
}

function registerMiss(kind: FailureKind) {
  misses++;
  if (kind === 'timeout') failTimeouts++; else failNeterrors++;
  if (misses >= MISS_LIMIT) _connected = false;
}

export function connect(ip: string) {
  if (timeoutId) { clearTimeout(timeoutId); timeoutId = null; }
  currentIP     = ip;
  _connected    = false;
  _lastData     = null;
  misses        = 0;
  failTimeouts  = 0;
  failNeterrors = 0;
  poll();                                          // immediate first poll; chains itself
}

export function disconnect() {
  currentIP     = '';
  _connected    = false;
  _lastData     = null;
  misses        = 0;
  failTimeouts  = 0;
  failNeterrors = 0;
  if (timeoutId) { clearTimeout(timeoutId); timeoutId = null; }
}

export function subscribe(fn: Listener): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function isConnected(): boolean {
  return _connected;
}

// Drains (returns and clears) the failed-poll tally for the current gap. The
// logger reads this on the reconnect frame: t=0,n=0 ⇒ the app wasn't polling
// (backgrounded); otherwise timeouts vs neterrors seen during the gap.
export function drainFailCounts(): { t: number; n: number } {
  const c = { t: failTimeouts, n: failNeterrors };
  failTimeouts  = 0;
  failNeterrors = 0;
  return c;
}

export function getLastData(): TelemetryData | null {
  return _lastData;
}

// IP currently being polled (''. if disconnected). Used by the telemetry
// logger to pull /history for gap backfill on reconnect.
export function getCurrentIP(): string {
  return currentIP;
}
