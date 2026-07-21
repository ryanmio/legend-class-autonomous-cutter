// anchorRegistry.ts
// Persisted per-boot wall-clock anchors: session_id → (boat uptime_ms, wall
// ms) captured while the app was live-polling that boot. The flash-log import
// flow (flightlogService) uses them to give a mission recovered AFTER a crash
// or field power-off true timestamps — the boat has no clock, and the RAM
// anchors the backfill path uses die with the app process. Any boot the app
// ever polled (even the few seconds of a go/no-go check) gets an anchor here;
// a never-polled boot has none and imports as relative time.
//
// Additive: nothing else reads this store; the live logger and /history
// backfill keep their own in-RAM anchors exactly as before.

import AsyncStorage from '@react-native-async-storage/async-storage';

export interface SessionAnchor {
  uptimeMs: number;   // boat uptime at the anchor frame
  wallMs: number;     // Date.now() when that frame arrived
}

const KEY = 'anchors:v1';
const MAX_SESSIONS = 20;          // prune oldest-by-wall so the store stays tiny
const PERSIST_EVERY_MS = 60_000;  // refresh cadence; first frame persists at once

let cache: Record<string, SessionAnchor> | null = null;
let loadPromise: Promise<Record<string, SessionAnchor>> | null = null;
let lastPersistAt = 0;

async function load(): Promise<Record<string, SessionAnchor>> {
  if (cache) return cache;
  if (!loadPromise) {
    loadPromise = (async () => {
      try {
        const raw = await AsyncStorage.getItem(KEY);
        const parsed = raw ? (JSON.parse(raw) as Record<string, SessionAnchor>) : {};
        cache = parsed && typeof parsed === 'object' ? parsed : {};
      } catch {
        cache = {};
      }
      return cache;
    })();
  }
  return loadPromise;
}

async function persist(): Promise<void> {
  if (!cache) return;
  const entries = Object.entries(cache);
  if (entries.length > MAX_SESSIONS) {
    entries.sort((a, b) => b[1].wallMs - a[1].wallMs);
    cache = Object.fromEntries(entries.slice(0, MAX_SESSIONS));
  }
  try { await AsyncStorage.setItem(KEY, JSON.stringify(cache)); } catch {}
}

// Called on every telemetry frame (from the auto engine). Cheap: updates the
// in-memory anchor always, touches AsyncStorage at most once a minute — a
// fresher anchor only sharpens timestamps by seconds of clock drift, so ~1/min
// is plenty and keeps writes negligible.
export function noteAnchor(sessionId: number, uptimeS: number, wallMs: number): void {
  void (async () => {
    const c = await load();
    // A session's FIRST anchor persists immediately — a boot the app saw for
    // only seconds (then got killed) must still import with true timestamps.
    const isNewSession = !(String(sessionId) in c);
    c[String(sessionId)] = { uptimeMs: uptimeS * 1000, wallMs };
    if (isNewSession || wallMs - lastPersistAt >= PERSIST_EVERY_MS) {
      lastPersistAt = wallMs;
      await persist();
    }
  })();
}

export async function getAnchor(sessionId: number): Promise<SessionAnchor | null> {
  const c = await load();
  return c[String(sessionId)] ?? null;
}
