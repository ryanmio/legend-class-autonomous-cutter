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

const POLL_MS       = 1000;
const TIMEOUT_MS    = 2000;
const MISS_LIMIT    = 2;      // consecutive failed polls before declaring disconnected
// While polls are FAILING, back off exponentially instead of re-polling every ~1 s.
// Each failed poll abandons a TCP connection; on a link too weak to deliver the
// teardown that leaves a half-open socket on the boat's single-client web server,
// which holds it ~5 s (HTTP_MAX_DATA_WAIT). ~1 new connection/s outruns that and
// jams the boat for everyone (see RECONNECT_STALL_REPORT.md). Backoff throttles the
// churn below the boat's shed rate. The cap is deliberately > the boat's 5 s hold so
// half-open sockets can't accumulate; kept low (8 s) because the boat is under
// active supervision and we want to re-poll soon after the link returns.
const BACKOFF_MAX_MS = 8000;

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
    // Re-chain. Only if nothing else (a new connect()/disconnect()) took over.
    if (currentIP === ip) {
      // Healthy link (misses === 0, reset on this poll's success): keep the fixed
      // ~1 Hz cadence — subtract the elapsed poll time so successive polls start
      // ~POLL_MS apart and don't alias the boat's 1 Hz stream. The first success
      // after an outage lands here, so a recovered link instantly returns to 1 Hz.
      // Failing link: back off exponentially (1→2→4→8 s, capped) to throttle the
      // connection churn that jams the boat. Flat delay after the poll (not
      // start-relative) so the inter-connection gap actually grows.
      const delay = misses === 0
        ? Math.max(0, POLL_MS - (Date.now() - startedAt))
        : Math.min(POLL_MS * 2 ** (misses - 1), BACKOFF_MAX_MS);
      timeoutId = setTimeout(poll, delay);
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
