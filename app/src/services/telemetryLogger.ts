// telemetryLogger.ts
// In-memory ring-buffer logger + automatic per-flight save.
//
// Two layers:
//   1. Recorder — start/stop/clear, in-memory rows, CSV builder. Same
//      public API as before; TelemetryScreen still controls it manually.
//   2. Auto engine (initAutoLogger) — always-on subscriber that
//      auto-starts the recorder when the boat is throttled up, and
//      auto-saves the captured rows as a "flight" in storage.
//
//      Store-and-sync: a WiFi dropout no longer ends the flight. When
//      telemetry resumes with the same session_id, the gap is detected
//      (the boat's uptime jumped past the last frame we saw) and the
//      missing records are pulled from the boat's /history ring and
//      merged in — so the flight log stays unbroken across the outage.
//      A flight is finalized only on a reboot (session_id change), a long
//      unrecovered absence (boat truly gone), or a manual stop.
//
// Persistence:
//   ${documentDirectory}flights/<id>.csv   — CSV body, one file per flight.
//   AsyncStorage 'flights:index'           — JSON array of FlightMeta
//                                            (id, timestamps, row count, derived stats).
//
// Legacy AsyncStorage layout (flight:<id> → CSV body) is migrated on
// first listFlights() call after upgrade.

import { Share } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import * as FileSystem from 'expo-file-system/legacy';
import { TelemetryData } from '../types';
import { subscribe, getCurrentIP } from './websocketService';
import { fetchHistorySince, HistoryRecord } from './historyService';

export interface LogRow extends TelemetryData {
  ts: number;             // ms since epoch when the frame was received
}

export interface FlightMeta {
  id: string;             // ISO-ish timestamp, also the filename stem
  startTs: number;        // ms epoch of first row
  endTs: number;          // ms epoch of last row
  rowCount: number;

  // Derived stats — present on flights saved by v2+, lazily filled for
  // migrated v1 flights. All optional so consumers must null-check.
  distanceM?: number;          // total over-ground distance (1.5 m threshold)
  maxSpeedKts?: number;
  autoMs?: number;             // wall time spent in MODE_AUTO
  manualMs?: number;
  failsafeMs?: number;
  battStartV?: number;
  battEndV?: number;
  maxDepthM?: number;
  capturedWaypoint?: boolean;
  gpsRowCount?: number;        // # rows with gps_fix and parseable lat/lon

  storage?: 'fs' | 'async';    // where the CSV body lives
}

const FLIGHTS_INDEX_KEY  = 'flights:index';
const LEGACY_FLIGHT_KEY  = 'flight:';  // AsyncStorage key prefix for v1 bodies
const FLIGHT_DIR         = (FileSystem.documentDirectory ?? '') + 'flights/';

// Cap at 7200 rows ≈ 2 hours @ 1 Hz. Drops oldest when full.
const MAX_ROWS = 7200;

// Auto-engine tuning.
const AUTO_ESC_DEADBAND_US   = 30;        // |esc_us - 1500| must exceed this
const AUTO_ESC_DEBOUNCE_MS   = 500;       // ...for this long to auto-start
// Boat truly gone → finalize so the flight isn't lost. Set well above any
// realistic WiFi dropout: a blip is bridged by /history backfill, not by
// ending the flight. Tied to the boat's ~20 min history ring — beyond it the
// gap can't be fully recovered anyway. Manual stop ends a flight immediately.
const AUTO_GONE_THRESHOLD_MS = 300_000;   // 5 min of no frames = boat is gone
const AUTO_TICK_MS           = 5_000;     // how often to check for telemetry loss
// Uptime jump (s) between two received frames that means we missed data and
// should backfill the hole from /history. Normal cadence is ~1 s/frame.
const GAP_TRIGGER_S          = 3;
const BACKFILL_MAX_PAGES     = 50;

// Stats tuning.
// GPS jitter floor: per-step displacements under this are noise, not real
// travel. Outdoor BN-220 typically jitters ±2 m even stationary.
const DISTANCE_STEP_MIN_M    = 1.5;
// Cap any single inter-row gap when attributing wall time to a mode.
// Beyond this, treat as a telemetry gap and don't credit any mode.
const MODE_TIME_GAP_CAP_MS   = 10_000;

let rows: LogRow[] = [];
let unsubscribe: (() => void) | null = null;
let countListeners:   Array<(count: number) => void>   = [];
let runningListeners: Array<(running: boolean) => void> = [];

function notifyCount()   { for (const fn of countListeners)   fn(rows.length);          }
function notifyRunning() { for (const fn of runningListeners) fn(unsubscribe !== null); }

export function start() {
  if (unsubscribe) return;
  unsubscribe = subscribe((data) => {
    rows.push({ ts: Date.now(), ...data });
    if (rows.length > MAX_ROWS) rows.splice(0, rows.length - MAX_ROWS);
    notifyCount();
  });
  notifyRunning();
}

// stop = "end this flight". Snapshots the buffer to a saved flight,
// then clears + halts the recorder. Safe to call when not running
// (no-op) and when the buffer is empty (no save, just halt).
export function stop() {
  if (!unsubscribe) return;
  unsubscribe();
  unsubscribe = null;
  notifyRunning();
  finalizeAndSave().catch((e) => console.warn('[telemetryLogger] save failed', e));
}

// clear = "throw away current rows without saving". Does NOT stop
// recording — matches the existing TelemetryScreen CLEAR button.
export function clear() {
  rows = [];
  notifyCount();
}

export function isRunning(): boolean {
  return unsubscribe !== null;
}

export function getRowCount(): number {
  return rows.length;
}

export function subscribeCount(fn: (count: number) => void): () => void {
  countListeners.push(fn);
  fn(rows.length);
  return () => { countListeners = countListeners.filter((l) => l !== fn); };
}

export function subscribeRunning(fn: (running: boolean) => void): () => void {
  runningListeners.push(fn);
  fn(unsubscribe !== null);
  return () => { runningListeners = runningListeners.filter((l) => l !== fn); };
}

// ── CSV building / parsing ─────────────────────────────────────────────────

function columns(forRows: LogRow[]): string[] {
  const seen = new Set<string>();
  for (const r of forRows) for (const k of Object.keys(r)) seen.add(k);
  const ordered = ['ts', 'v'];
  const rest = Array.from(seen).filter((k) => !ordered.includes(k)).sort();
  return [...ordered, ...rest];
}

function escapeCell(v: unknown): string {
  if (v === undefined || v === null) return '';
  const s = String(v);
  if (s.includes(',') || s.includes('"') || s.includes('\n')) {
    return `"${s.replace(/"/g, '""')}"`;
  }
  return s;
}

function rowsToCSV(forRows: LogRow[]): string {
  if (forRows.length === 0) return '';
  const cols = columns(forRows);
  const header = cols.join(',');
  const body = forRows.map((r) =>
    cols.map((c) => {
      if (c === 'ts') return new Date(r.ts).toISOString();
      return escapeCell((r as unknown as Record<string, unknown>)[c]);
    }).join(',')
  );
  return [header, ...body].join('\n');
}

// Single-line CSV parser. Our writer never emits a newline inside a
// cell (telemetry fields are numbers / booleans / short enums), so it's
// safe to split the file on '\n' first.
function parseCSVLine(line: string): string[] {
  const cells: string[] = [];
  let cur = '';
  let inQ = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (inQ) {
      if (c === '"') {
        if (line[i + 1] === '"') { cur += '"'; i++; continue; }
        inQ = false;
        continue;
      }
      cur += c;
    } else {
      if (c === ',') { cells.push(cur); cur = ''; continue; }
      if (c === '"' && cur === '') { inQ = true; continue; }
      cur += c;
    }
  }
  cells.push(cur);
  return cells;
}

export function parseCSV(csv: string): Array<Record<string, string>> {
  const lines = csv.split('\n');
  if (lines.length < 2) return [];
  const header = parseCSVLine(lines[0]);
  const out: Array<Record<string, string>> = [];
  for (let i = 1; i < lines.length; i++) {
    if (lines[i] === '') continue;
    const cells = parseCSVLine(lines[i]);
    const row: Record<string, string> = {};
    for (let j = 0; j < header.length; j++) row[header[j]] = cells[j] ?? '';
    out.push(row);
  }
  return out;
}

// Builds CSV from the current in-memory buffer — kept for backward
// compatibility with code that calls getCSV() directly. New code
// should prefer the saved-flight flow (saveFlight / loadFlight).
export function getCSV(): string {
  return rowsToCSV(rows);
}

// Pop a Share sheet with the current in-memory buffer as a string
// message. Same behavior as before. Used by TelemetryScreen's EXPORT
// button. Saved flights have their own export path in FlightDetailScreen.
export async function exportShare(): Promise<void> {
  if (rows.length === 0) {
    await Share.share({ message: 'No telemetry rows captured yet.' });
    return;
  }
  const csv = getCSV();
  const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
  await Share.share({
    title: `legend-cutter-telemetry-${stamp}.csv`,
    message: csv,
  });
}

// ── Stats computation ──────────────────────────────────────────────────────

function num(v: unknown): number | null {
  if (v == null || v === '') return null;
  const n = typeof v === 'number' ? v : parseFloat(String(v));
  return Number.isFinite(n) ? n : null;
}

function asBool(v: unknown): boolean {
  return v === true || v === 'true';
}

function tsMs(v: unknown): number {
  if (typeof v === 'number') return v;
  const s = String(v ?? '');
  const parsed = Date.parse(s);
  return Number.isFinite(parsed) ? parsed : 0;
}

function haversineMeters(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const R = 6371000;
  const φ1 = (lat1 * Math.PI) / 180;
  const φ2 = (lat2 * Math.PI) / 180;
  const Δφ = ((lat2 - lat1) * Math.PI) / 180;
  const Δλ = ((lon2 - lon1) * Math.PI) / 180;
  const a = Math.sin(Δφ / 2) ** 2 + Math.cos(φ1) * Math.cos(φ2) * Math.sin(Δλ / 2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

type StatsInput = Array<Record<string, unknown>>;

export interface FlightStats {
  distanceM: number;
  maxSpeedKts: number;
  autoMs: number;
  manualMs: number;
  failsafeMs: number;
  battStartV?: number;
  battEndV?: number;
  maxDepthM: number;
  capturedWaypoint: boolean;
  gpsRowCount: number;
}

export function computeStats(rs: StatsInput): FlightStats {
  let distanceM       = 0;
  let maxSpeedKts     = 0;
  let autoMs          = 0;
  let manualMs        = 0;
  let failsafeMs      = 0;
  let battStartV: number | undefined;
  let battEndV:   number | undefined;
  let maxDepthM       = 0;
  let capturedWaypoint = false;
  let gpsRowCount     = 0;

  let prevGps: { lat: number; lon: number } | null = null;
  let prevTs:   number | null = null;
  let prevMode: string | null = null;

  for (const r of rs) {
    const t = tsMs(r.ts);

    // Time-in-mode (credit dt to mode at start of interval, cap gaps).
    const mode = r.mode == null ? null : String(r.mode);
    if (prevTs != null && prevMode != null) {
      const dt = t - prevTs;
      if (dt > 0 && dt < MODE_TIME_GAP_CAP_MS) {
        if      (prevMode === 'AUTO')     autoMs     += dt;
        else if (prevMode === 'MANUAL')   manualMs   += dt;
        else if (prevMode === 'FAILSAFE') failsafeMs += dt;
      }
    }
    prevTs   = t;
    prevMode = mode;

    // GPS-derived distance (jitter floor).
    if (asBool(r.gps_fix)) {
      const lat = num(r.lat);
      const lon = num(r.lon);
      if (lat != null && lon != null) {
        gpsRowCount++;
        if (prevGps != null) {
          const step = haversineMeters(prevGps.lat, prevGps.lon, lat, lon);
          if (step >= DISTANCE_STEP_MIN_M) {
            distanceM += step;
            prevGps = { lat, lon };
          }
          // else: leave prevGps anchored so slow drift eventually clears
          //       the threshold against the original sample.
        } else {
          prevGps = { lat, lon };
        }
      }
    }

    const speed = num(r.speed_kts);
    if (speed != null && speed > maxSpeedKts) maxSpeedKts = speed;

    const batt = num(r.batt_v);
    if (batt != null) {
      if (battStartV === undefined) battStartV = batt;
      battEndV = batt;
    }

    const depth = num(r.depth_m);
    if (depth != null && depth > maxDepthM) maxDepthM = depth;

    if (asBool(r.captured)) capturedWaypoint = true;
  }

  return {
    distanceM:        Math.round(distanceM * 10) / 10,
    maxSpeedKts:      Math.round(maxSpeedKts * 100) / 100,
    autoMs,
    manualMs,
    failsafeMs,
    battStartV:       battStartV !== undefined ? Math.round(battStartV * 100) / 100 : undefined,
    battEndV:         battEndV   !== undefined ? Math.round(battEndV   * 100) / 100 : undefined,
    maxDepthM:        Math.round(maxDepthM * 100) / 100,
    capturedWaypoint,
    gpsRowCount,
  };
}

// Extract GPS track (and ts) from a parsed CSV — used by FlightDetailScreen
// to draw the polyline.
export interface TrackPoint { ts: number; lat: number; lon: number }
export function extractTrack(rs: StatsInput): TrackPoint[] {
  const out: TrackPoint[] = [];
  for (const r of rs) {
    if (!asBool(r.gps_fix)) continue;
    const lat = num(r.lat);
    const lon = num(r.lon);
    if (lat == null || lon == null) continue;
    out.push({ ts: tsMs(r.ts), lat, lon });
  }
  return out;
}

// ── Filesystem helpers ─────────────────────────────────────────────────────

async function ensureFlightDir(): Promise<void> {
  const info = await FileSystem.getInfoAsync(FLIGHT_DIR);
  if (!info.exists) await FileSystem.makeDirectoryAsync(FLIGHT_DIR, { intermediates: true });
}

function flightPath(id: string): string {
  return FLIGHT_DIR + id + '.csv';
}

export function getFlightFileUri(id: string): string {
  return flightPath(id);
}

async function writeFlightFile(id: string, csv: string): Promise<void> {
  await ensureFlightDir();
  await FileSystem.writeAsStringAsync(flightPath(id), csv);
}

async function readFlightFile(id: string): Promise<string | null> {
  const info = await FileSystem.getInfoAsync(flightPath(id));
  if (!info.exists) return null;
  return FileSystem.readAsStringAsync(flightPath(id));
}

async function deleteFlightFile(id: string): Promise<void> {
  await FileSystem.deleteAsync(flightPath(id), { idempotent: true });
}

// ── Saved flights (index in AsyncStorage, bodies on disk) ─────────────────

async function readIndex(): Promise<FlightMeta[]> {
  const raw = await AsyncStorage.getItem(FLIGHTS_INDEX_KEY);
  if (!raw) return [];
  try {
    const arr = JSON.parse(raw) as FlightMeta[];
    return Array.isArray(arr) ? arr : [];
  } catch {
    return [];
  }
}

async function writeIndex(index: FlightMeta[]): Promise<void> {
  await AsyncStorage.setItem(FLIGHTS_INDEX_KEY, JSON.stringify(index));
}

// One-shot migration. For every meta entry without `storage: 'fs'`:
//   • read CSV from AsyncStorage key 'flight:<id>'
//   • write to filesystem
//   • compute stats from the CSV
//   • update meta in place (storage='fs' + stats)
//   • remove the AsyncStorage body
// Idempotent: a partially-migrated flight will retry on next call.
// Memoize per-session — runs at most once per app launch. Concurrent
// callers share the in-flight promise.
let migratePromise: Promise<void> | null = null;
async function migrateLegacyFlights(): Promise<void> {
  if (migratePromise) return migratePromise;
  migratePromise = (async () => {
    const index = await readIndex();
    let mutated = false;
    for (let i = 0; i < index.length; i++) {
      const m = index[i];
      if (m.storage === 'fs') continue;

      const legacyKey = LEGACY_FLIGHT_KEY + m.id;
      let csv: string | null = null;
      try {
        csv = await AsyncStorage.getItem(legacyKey);
      } catch (e) {
        console.warn('[telemetryLogger] legacy read failed', m.id, e);
      }

      // If body already on disk (interrupted prior migration), skip the write.
      const existing = await readFlightFile(m.id);
      if (existing != null) csv = existing;

      if (csv == null) {
        // No body anywhere — orphaned meta. Mark fs to stop retrying;
        // listFlights will simply fail to read it later.
        index[i] = { ...m, storage: 'fs' };
        mutated = true;
        continue;
      }

      try {
        if (existing == null) await writeFlightFile(m.id, csv);
        const stats = computeStats(parseCSV(csv));
        index[i] = { ...m, ...stats, storage: 'fs' };
        mutated = true;
        try { await AsyncStorage.removeItem(legacyKey); } catch {}
      } catch (e) {
        console.warn('[telemetryLogger] migration failed', m.id, e);
      }
    }
    if (mutated) await writeIndex(index);
  })();
  return migratePromise;
}

export async function listFlights(): Promise<FlightMeta[]> {
  await migrateLegacyFlights();
  return readIndex();
}

export async function loadFlightCSV(id: string): Promise<string | null> {
  await migrateLegacyFlights();
  const onDisk = await readFlightFile(id);
  if (onDisk != null) return onDisk;
  // Fallback: very stale meta that pre-dates migration code paths.
  return AsyncStorage.getItem(LEGACY_FLIGHT_KEY + id);
}

export async function loadFlightRows(id: string): Promise<Array<Record<string, string>>> {
  const csv = await loadFlightCSV(id);
  if (csv == null) return [];
  return parseCSV(csv);
}

export async function deleteFlight(id: string): Promise<void> {
  await deleteFlightFile(id);
  try { await AsyncStorage.removeItem(LEGACY_FLIGHT_KEY + id); } catch {}
  const index = await readIndex();
  await writeIndex(index.filter((m) => m.id !== id));
}

// Snapshots `rows` into a saved flight if non-empty, then clears the
// in-memory buffer. Idempotent: safe to call on an empty buffer (no-op).
async function finalizeAndSave(): Promise<void> {
  if (rows.length === 0) return;
  const snapshot = rows;
  // Reset state before the async write so a new flight can start
  // building without racing with the save.
  rows = [];
  notifyCount();

  const startTs = snapshot[0].ts;
  const endTs   = snapshot[snapshot.length - 1].ts;
  const id      = new Date(startTs).toISOString().replace(/[:.]/g, '-').slice(0, 19);
  const csv     = rowsToCSV(snapshot);
  const stats   = computeStats(snapshot as unknown as StatsInput);

  await writeFlightFile(id, csv);
  const index = await readIndex();
  index.push({ id, startTs, endTs, rowCount: snapshot.length, storage: 'fs', ...stats });
  await writeIndex(index);
}

// ── Auto engine ────────────────────────────────────────────────────────────

let autoInitialized      = false;
let autoLastSessionId: number | null = null;
let autoLastFrameAt      = 0;
let autoEscAboveSince: number | null = null;
let autoTickHandle: ReturnType<typeof setInterval> | null = null;
// uptime (s) of the previous frame we saw — used to spot a gap on reconnect.
let autoPrevUptimeS: number | null = null;
let backfillInProgress   = false;

function autoOnFrame(data: TelemetryData) {
  const now = Date.now();
  autoLastFrameAt = now;

  // 1. Reboot detection (session_id changed). A reboot is a hard flight
  //    boundary — the boat's history ring is cleared, so we can't backfill
  //    across it. Finalize the pre-reboot flight.
  let rebooted = false;
  if (data.session_id != null) {
    if (autoLastSessionId != null && data.session_id !== autoLastSessionId) {
      rebooted = true;
      if (unsubscribe) stop();  // saves current flight, halts recorder
    }
    autoLastSessionId = data.session_id;
  }
  if (rebooted) autoPrevUptimeS = null;

  // 2. Throttle-up detection — ESC commanded out of neutral for the
  //    debounce window. Uses the boat's commanded ESC µs (not raw CH3)
  //    so MANUAL forward, MANUAL reverse-via-right-stick, and AUTO
  //    cruise all trigger uniformly.
  let justStarted = false;
  if (data.esc_us != null && Math.abs(data.esc_us - 1500) > AUTO_ESC_DEADBAND_US) {
    if (autoEscAboveSince == null) autoEscAboveSince = now;
    if (!unsubscribe && (now - autoEscAboveSince) >= AUTO_ESC_DEBOUNCE_MS) {
      clear();   // ensure clean buffer for the new flight
      start();
      justStarted = true;  // don't backfill pre-flight history into a fresh flight
    }
  } else {
    autoEscAboveSince = null;
  }

  // 3. Gap backfill — if a flight is running and this frame's uptime jumped
  //    well past the previous frame's, we missed data (a WiFi dropout we just
  //    recovered from). Pull the hole from the boat's /history ring and merge
  //    it in. This frame is the wall-clock anchor for converting record
  //    uptimes to timestamps.
  if (
    unsubscribe && !rebooted && !justStarted &&
    data.uptime != null && autoPrevUptimeS != null &&
    data.uptime - autoPrevUptimeS > GAP_TRIGGER_S
  ) {
    void backfillGap(autoPrevUptimeS * 1000, data.uptime * 1000, now, data.session_id ?? null, data.v);
  }

  if (data.uptime != null) autoPrevUptimeS = data.uptime;
}

// Pull every record in (sinceMs, now] from the boat and merge the ones we're
// missing into the running flight. Guarded so only one runs at a time.
async function backfillGap(
  sinceMs: number,
  anchorUptimeMs: number,
  anchorWallMs: number,
  expectSessionId: number | null,
  version?: string,
): Promise<void> {
  if (backfillInProgress) return;
  const ip = getCurrentIP();
  if (!ip) return;
  backfillInProgress = true;
  try {
    const { sessionId, records } = await fetchHistorySince(ip, sinceMs, BACKFILL_MAX_PAGES);
    // If the boat rebooted between the anchor frame and our fetch, the ring is
    // a different flight — don't merge across the boundary.
    if (expectSessionId != null && sessionId !== expectSessionId) return;
    if (!unsubscribe) return;  // flight ended (e.g. manual stop) while fetching
    mergeBackfill(records, anchorUptimeMs, anchorWallMs, version);
  } catch (e) {
    console.warn('[telemetryLogger] backfill failed', e);
  } finally {
    backfillInProgress = false;
  }
}

// Insert backfilled records into `rows`, skipping any second already covered by
// a live frame, then re-sort by timestamp. Each record's wall-clock ts is
// derived from the anchor frame: ts = anchorWall - (anchorUptime - recUptime).
function mergeBackfill(
  records: HistoryRecord[],
  anchorUptimeMs: number,
  anchorWallMs: number,
  version?: string,
): void {
  if (records.length === 0) return;

  const seenSec = new Set<number>();
  for (const r of rows) {
    const u = (r as unknown as { uptime?: number }).uptime;
    if (typeof u === 'number') seenSec.add(Math.round(u));
  }

  let added = 0;
  for (const rec of records) {
    const sec = Math.round(rec.uptime_ms / 1000);
    if (seenSec.has(sec)) continue;  // already have a live frame for this second
    seenSec.add(sec);

    const { seq, uptime_ms, ...fields } = rec;
    void seq;
    const ts = anchorWallMs - (anchorUptimeMs - uptime_ms);
    const row = { ...fields, ts, uptime: sec, v: version ?? '' } as unknown as LogRow;
    rows.push(row);
    added++;
  }

  if (added === 0) return;
  rows.sort((a, b) => a.ts - b.ts);
  if (rows.length > MAX_ROWS) rows.splice(0, rows.length - MAX_ROWS);
  notifyCount();
}

function autoTick() {
  // Boat-gone detection. Only relevant while a flight is running. A short/
  // medium WiFi dropout is bridged by /history backfill, not by ending the
  // flight, so the threshold is deliberately long.
  if (!unsubscribe) return;
  if (autoLastFrameAt === 0) return;
  if (Date.now() - autoLastFrameAt > AUTO_GONE_THRESHOLD_MS) {
    stop();  // saves the flight
  }
}

// Wire the auto engine to the telemetry stream. Idempotent — safe to
// call multiple times. Call once from App.tsx on mount.
export function initAutoLogger() {
  if (autoInitialized) return;
  autoInitialized = true;
  subscribe(autoOnFrame);
  autoTickHandle = setInterval(autoTick, AUTO_TICK_MS);
}

// Test-only / shutdown helper. Not used by the app today but kept so
// the tick interval can be torn down cleanly if needed.
export function shutdownAutoLogger() {
  if (autoTickHandle) { clearInterval(autoTickHandle); autoTickHandle = null; }
  autoInitialized = false;
}
