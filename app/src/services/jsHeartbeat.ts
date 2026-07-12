// jsHeartbeat.ts
// Liveness probe for the JS thread. A fixed 250 ms interval measures its own
// lateness against the wall clock. If it fires 9 s late, the JS timer queue went
// unserviced for 9 s — which is precisely the failure the telemetry poll loop
// shows: its own 2 s abort timer never fires, so a multi-second stall registers
// zero failed polls (poll_fail "t0 n0") and the gap looks like a network outage
// when no request was ever issued.
//
// Two discriminators are captured with each freeze:
//
//   state — AppState sampled on the last tick that ran ON TIME, i.e. going into
//     the freeze, paired with the state on recovery. iOS suspending the app and a
//     synchronous JS block both look identical from the late tick alone (by then
//     we are 'active' again); only the before-state separates them.
//   busy — whether the logger's checkpoint (synchronous rowsToCSV + a file write
//     that reaches ~half a megabyte) or the /history backfill (up to 50 pages of
//     JSON.parse plus a full re-sort of rows) was in flight going into the freeze.
//     This is the bisect: it names the blocker in the same run that detects it.
//
// Diagnostic only — records, changes nothing.

import { AppState } from 'react-native';

const TICK_MS       = 250;
const FREEZE_MIN_MS = 400;   // lateness above this is a stall, not scheduler jitter

let handle: ReturnType<typeof setInterval> | null = null;
let expectedAt = 0;

// Sampled on the last on-time tick — the state of the world going INTO a freeze.
let lastGoodState = 'active';
let lastGoodBusy  = 'none';

// Worst freeze since the last drain.
let maxLateMs   = 0;
let freezeCount = 0;
let maxLateDesc = '';

let busyCheckpoint = false;
let busyBackfill   = false;

function busyTag(): string {
  if (busyCheckpoint && busyBackfill) return 'both';
  if (busyCheckpoint) return 'checkpoint';
  if (busyBackfill)   return 'backfill';
  return 'none';
}

// Marks a suspected JS-thread blocker as in-flight. Tracked as two independent
// flags rather than one string so an overlapping checkpoint and backfill can't
// clear each other's tag.
export function setBusy(tag: 'checkpoint' | 'backfill', on: boolean): void {
  if (tag === 'checkpoint') busyCheckpoint = on;
  else                      busyBackfill   = on;
}

export function startHeartbeat(): void {
  if (handle) return;
  expectedAt = Date.now() + TICK_MS;
  handle = setInterval(() => {
    const now  = Date.now();
    const late = now - expectedAt;
    // Re-anchor to now, so one freeze reports once instead of cascading into
    // every tick that follows it.
    expectedAt = now + TICK_MS;

    if (late < FREEZE_MIN_MS) {
      lastGoodState = AppState.currentState;
      lastGoodBusy  = busyTag();
      return;
    }

    const desc = `state=${lastGoodState}>${AppState.currentState} busy=${lastGoodBusy}`;
    freezeCount++;
    if (late > maxLateMs) {
      maxLateMs   = late;
      maxLateDesc = desc;
    }
    console.warn(`[jsHeartbeat] JS timers ${late}ms late — ${desc}`);
  }, TICK_MS);
}

// Worst JS-timer freeze since the last call, or null if timers ran clean. The
// flight logger stamps this on the next recorded row, so a freeze lands in the
// exported CSV next to poll_fail whether or not it opened a telemetry gap — and
// a freeze that happens before the first row (right after connect) lands on the
// first row of the flight.
export function drainFreeze(): string | null {
  if (freezeCount === 0) return null;
  const s = `late=${maxLateMs} ${maxLateDesc} n=${freezeCount}`;
  maxLateMs   = 0;
  freezeCount = 0;
  maxLateDesc = '';
  return s;
}
