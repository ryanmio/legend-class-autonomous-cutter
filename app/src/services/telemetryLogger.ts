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

import { Share, AppState, AppStateStatus } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import * as FileSystem from 'expo-file-system/legacy';
import * as Sharing from 'expo-sharing';
import { TelemetryData } from '../types';
import { subscribe, getCurrentIP, drainFailCounts } from './websocketService';
import { startHeartbeat, drainFreeze, setBusy } from './jsHeartbeat';
import { fetchHistorySince, HistoryRecord } from './historyService';
import { HISTORY_RING_CAPACITY } from '../constants';

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
// Crash-safety: the in-progress flight is mirrored here on a timer while
// recording, so a kill/eviction before finalize loses at most the last
// checkpoint interval (not the whole flight). Cleared on finalize/clear,
// recovered into a real flight on next launch. Not in the flights index, so
// it never shows in the flight list. Leading underscore keeps it out of the
// way of timestamp-named flight files.
const DRAFT_PATH         = FLIGHT_DIR + '_draft.csv';

// Cap at 7200 rows ≈ 2 hours @ 1 Hz. Drops oldest when full.
const MAX_ROWS = 7200;

// Auto-engine tuning.
const AUTO_ESC_DEADBAND_US   = 30;        // |esc_us - 1500| must exceed this
const AUTO_ESC_DEBOUNCE_MS   = 500;       // ...for this long to auto-start
// Boat truly gone → finalize so the flight isn't lost. Set to the boat's full
// ~20 min history ring: a WiFi dropout (or the app being backgrounded for the
// camera) is bridged by /history backfill, not by ending the flight, so this
// only needs to fire when the boat is genuinely gone. Crash-safety is the 30 s
// draft checkpoint + launch recovery, independent of this — so a longer window
// costs no data. Only judged while foregrounded (see autoTick). Manual STOP
// ends a flight immediately.
const AUTO_GONE_THRESHOLD_MS = 1_200_000;   // 20 min of no frames = boat is gone
const AUTO_TICK_MS           = 5_000;     // how often to check for telemetry loss
// Uptime jump (s) between two received frames that means we missed data and
// should backfill the hole from /history. Normal cadence is ~1 s/frame.
const GAP_TRIGGER_S          = 3;
const BACKFILL_MAX_PAGES     = 50;
// How often the running flight is mirrored to the draft file (crash-safety).
const CHECKPOINT_MS          = 30_000;

// Stats tuning.
// GPS jitter floor: per-step displacements under this are noise, not real
// travel. Outdoor BN-220 typically jitters ±2 m even stationary.
const DISTANCE_STEP_MIN_M    = 1.5;
// Cap any single inter-row gap when attributing wall time to a mode.
// Beyond this, treat as a telemetry gap and don't credit any mode.
const MODE_TIME_GAP_CAP_MS   = 10_000;

let rows: LogRow[] = [];
// Boat-seconds already present in `rows`, keyed by floor(uptime) — shared by BOTH
// the live-append and backfill paths so a second can't enter twice. Kept in sync
// with `rows`: added at each push, cleared whenever `rows` is emptied. (Trimming
// old rows leaves stale entries, which is harmless — uptime is monotonic within a
// flight, so a trimmed second never legitimately recurs.)
let seenSeconds = new Set<number>();
let unsubscribe: (() => void) | null = null;
let countListeners:   Array<(count: number) => void>   = [];
let runningListeners: Array<(running: boolean) => void> = [];

function notifyCount()   { for (const fn of countListeners)   fn(rows.length);          }
function notifyRunning() { for (const fn of runningListeners) fn(unsubscribe !== null); }

export function start() {
  if (unsubscribe) return;
  lastCheckpointAt = 0;   // checkpoint on the next tick, not 30 s in
  pendingGaps = [];       // a fresh flight inherits no stale gaps
  pendingPollFail = null;
  unsubscribe = subscribe((data) => {
    // Per-second dedup, shared with the backfill path: a boat-second the backfill
    // already merged (its /history read can reach past the last live frame) must
    // not be re-added when its live frame arrives a beat later, or the seam gets a
    // delta-0 duplicate. Key on floor(uptime) — the same convention mergeBackfill
    // now uses — so the two routes key identically. Check before consuming
    // pendingPollFail so a skipped dup can't drop a reconnect marker (dup frames
    // are never reconnect frames anyway).
    const sec = typeof data.uptime === 'number' ? Math.floor(data.uptime) : null;
    if (sec !== null && seenSeconds.has(sec)) return;
    if (sec !== null) seenSeconds.add(sec);

    const row = { ts: Date.now(), ...data } as LogRow;
    // If autoOnFrame just flagged this frame as a reconnect (it runs first —
    // subscribed at init, before this recorder), stamp the app's failed-poll
    // tally over the gap. A blank poll_fail ⇒ not a reconnect row; "t0 n0" ⇒
    // reconnect but the app wasn't polling. Pairs with the boat-side wifi_assoc
    // on the backfilled rows.
    if (pendingPollFail !== null) {
      (row as unknown as Record<string, unknown>).poll_fail = pendingPollFail;
      pendingPollFail = null;
    }
    // Worst JS-timer freeze since the previous row. Drained per-row rather than
    // per-gap so a freeze is captured even when it opened no telemetry gap — and
    // so one that happens before the first row (a stall right after connect) lands
    // on the first row of the flight instead of being lost.
    const freeze = drainFreeze();
    if (freeze !== null) {
      (row as unknown as Record<string, unknown>).js_freeze = freeze;
    }
    rows.push(row);
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
  seenSeconds.clear();             // dedup set mirrors rows
  deleteDraft().catch(() => {});   // don't let a stale draft resurrect discarded rows
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
  // Prefer a real .csv file attachment over a text-message body: a long
  // buffer pasted as message text is truncated/mangled by most share
  // targets and never lands as an openable file. Mirrors the saved-flight
  // export in FlightDetailScreen. Falls back to a text message on
  // platforms without the native share sheet (web).
  try {
    if (await Sharing.isAvailableAsync()) {
      await ensureFlightDir();
      const uri = FLIGHT_DIR + `_export_${stamp}.csv`;
      await FileSystem.writeAsStringAsync(uri, csv);
      try {
        await Sharing.shareAsync(uri, {
          mimeType: 'text/csv',
          UTI: 'public.comma-separated-values-text',
          dialogTitle: `legend-cutter-telemetry-${stamp}.csv`,
        });
      } finally {
        await FileSystem.deleteAsync(uri, { idempotent: true }).catch(() => {});
      }
      return;
    }
  } catch {
    // fall through to the text-message share below
  }
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

    // captured now means "whole mission complete"; wp_idx advancing past 0 means
    // at least one leg was captured. Either counts as "this flight captured a WP",
    // so a multi-leg mission aborted before the final leg still shows the badge.
    if (asBool(r.captured) || (num(r.wp_idx) ?? 0) > 0) capturedWaypoint = true;
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
  seenSeconds.clear();             // dedup set mirrors rows
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
  await deleteDraft();   // the flight is now persisted properly
}

// ── Crash-safety draft (mirror in-progress flight to disk) ──────────────────

// Best-effort mirror of the current buffer. Called on a timer while recording.
async function checkpointDraft(): Promise<void> {
  if (rows.length === 0) return;
  setBusy('checkpoint', true);
  try {
    await ensureFlightDir();
    await FileSystem.writeAsStringAsync(DRAFT_PATH, rowsToCSV(rows));
  } catch (e) {
    console.warn('[telemetryLogger] draft checkpoint failed', e);
  } finally {
    setBusy('checkpoint', false);
  }
}

async function deleteDraft(): Promise<void> {
  try { await FileSystem.deleteAsync(DRAFT_PATH, { idempotent: true }); } catch {}
}

// On launch, turn any leftover draft (app killed mid-flight last time) into a
// real saved flight, then remove it. Idempotent: if a flight with the derived
// id already exists, just drop the draft.
async function recoverDraft(): Promise<void> {
  let csv: string | null = null;
  try {
    const info = await FileSystem.getInfoAsync(DRAFT_PATH);
    if (!info.exists) return;
    csv = await FileSystem.readAsStringAsync(DRAFT_PATH);
  } catch {
    return;
  }
  const parsed = csv ? parseCSV(csv) : [];
  if (parsed.length === 0) { await deleteDraft(); return; }

  try {
    const startTs = tsMs(parsed[0].ts);
    const endTs   = tsMs(parsed[parsed.length - 1].ts);
    const id      = new Date(startTs).toISOString().replace(/[:.]/g, '-').slice(0, 19);
    const index   = await readIndex();
    if (!index.some((m) => m.id === id)) {
      await writeFlightFile(id, csv as string);
      const stats = computeStats(parsed);
      index.push({ id, startTs, endTs, rowCount: parsed.length, storage: 'fs', ...stats });
      await writeIndex(index);
    }
  } catch (e) {
    console.warn('[telemetryLogger] draft recovery failed', e);
    return;   // leave the draft for a future retry rather than losing it
  }
  await deleteDraft();
}

// ── Auto engine ────────────────────────────────────────────────────────────

let autoInitialized      = false;
let autoLastSessionId: number | null = null;
// Wall-clock ms of the last real telemetry frame received. Written ONLY by
// autoOnFrame — never bumped on foreground — so it is a true "last contact"
// timestamp for the Telemetry display (lastFrameAt).
let autoLastFrameAt      = 0;
// Foreground-grace anchor for the boat-gone detector ONLY. Bumped when the app
// returns to the foreground while recording, so a backgrounded app's frozen
// poll timer (its silence) isn't misread as "boat gone". Kept separate from
// autoLastFrameAt so the display timer never jumps on foreground/navigation.
let autoForegroundedAt   = 0;
let autoEscAboveSince: number | null = null;
let autoTickHandle: ReturnType<typeof setInterval> | null = null;
// uptime (s) of the previous frame we saw — used to spot a gap on reconnect.
let autoPrevUptimeS: number | null = null;
// Set by autoOnFrame when it detects a reconnect gap; consumed by the row
// recorder to tag the reconnect frame with the app's failed-poll tally over the
// gap ("t<timeouts> n<neterrors>"; "t0 n0" ⇒ the app wasn't polling).
let pendingPollFail: string | null = null;

// Pending gap backfills. A gap is detected the instant a frame's uptime jumps
// past the previous frame's, but the /history fetch that fills it can fail
// (marginal WiFi right after the boat re-enters range is exactly when it's
// invoked). Previously that was a fire-and-forget fetch whose error was
// swallowed and never retried — so one failed fetch lost the whole gap
// permanently, because autoPrevUptimeS advanced regardless. Now each gap is
// queued with its own wall-clock anchor and pumped sequentially, retried every
// tick until the link returns and the fetch succeeds. A gap is dropped only if
// the boat rebooted across it (ring cleared) or the flight ended.
interface PendingGap {
  sinceMs: number;          // pull /history records with uptime_ms > this
  anchorUptimeMs: number;   // reconnect frame uptime (ms) — ts conversion anchor
  anchorWallMs: number;     // reconnect frame wall time — ts conversion anchor
  sessionId: number | null; // boat session at detection; reboot guard
  version?: string;
  attempts: number;
  totalEst: number;         // estimated rows in this gap (≈ its span in seconds,
                            // capped at the ring depth) — the "of ~Y" denominator
}
let pendingGaps: PendingGap[] = [];
let pumpRunning = false;

// Sync-progress display state for the gap currently being pumped. syncSynced is
// a running count of rows CONFIRMED RECEIVED this attempt; it resets to 0 at the
// start of each gap attempt (a failed attempt refetches from sinceMs, so prior
// rows are re-pulled, not added — a resetting count is the honest "bad link,
// looping" signal). Pure display: never advances any watermark. Sampled by the
// Telemetry screen's 1 Hz tick, like pendingGapCount — no listener needed.
let syncSynced   = 0;
let syncTotalEst = 0;

// Exposed so a future UI affordance can show "telemetry gap, recovering".
export function pendingGapCount(): number { return pendingGaps.length; }
// Rows confirmed-received vs estimated total for the in-flight backfill, or null
// when nothing is pending. synced is clamped ≤ total (the ~Y estimate is fuzzy);
// completion is driven off pendingGapCount()→0, never off synced reaching total.
export function syncProgress(): { synced: number; total: number } | null {
  if (pendingGaps.length === 0) return null;
  return { synced: syncSynced, total: syncTotalEst };
}
// Wall-clock ms of the last real frame received (0 until the first frame).
// Frame-only — never bumped on foreground — so the Telemetry "time since
// contact lost" display reflects true continuous elapsed time and survives
// backgrounding and in-app navigation. Read-only; alters no behavior.
export function lastFrameAt(): number { return autoLastFrameAt; }
// Foreground state. While the app is backgrounded (e.g. you switch to the
// camera) iOS freezes its poll timer, so it stops hearing the boat even though
// the boat is fine and still recording. We must not mistake that silence for
// "boat gone": the gap is backfilled frame-driven once polling resumes, exactly
// like a WiFi dropout. See onAppStateChange + autoTick.
let appState: AppStateStatus = 'active';
let appStateSub: { remove: () => void } | null = null;

function onAppStateChange(next: AppStateStatus) {
  const wasActive = appState === 'active';
  appState = next;
  if (next === 'active' && !wasActive && unsubscribe) {
    // Returned to foreground: the silence while away wasn't the boat leaving,
    // so give the boat-gone detector a fresh window via its own grace anchor
    // (not autoLastFrameAt — that must stay a true last-contact timestamp). The
    // missed records (incl. depth) are pulled in when the next poll frame
    // arrives and autoOnFrame sees the uptime jump.
    autoForegroundedAt = Date.now();
  }
}

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
  if (rebooted) { autoPrevUptimeS = null; pendingGaps = []; }

  // 2. Throttle-up detection — ESC commanded out of neutral for the
  //    debounce window. Uses the boat's commanded ESC µs (not raw CH3)
  //    so MANUAL forward, MANUAL reverse-via-right-stick, and AUTO
  //    cruise all trigger uniformly.
  let justStarted = false;
  if (data.port_us != null && Math.abs(data.port_us - 1500) > AUTO_ESC_DEADBAND_US) {
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
  //    recovered from). Queue the hole; the pump pulls it from the boat's
  //    /history ring and retries until it succeeds. This frame is the
  //    wall-clock anchor for converting the gap's record uptimes to timestamps.
  if (
    unsubscribe && !rebooted && !justStarted &&
    data.uptime != null && autoPrevUptimeS != null &&
    data.uptime - autoPrevUptimeS > GAP_TRIGGER_S
  ) {
    pendingGaps.push({
      sinceMs: autoPrevUptimeS * 1000,
      anchorUptimeMs: data.uptime * 1000,
      anchorWallMs: now,
      sessionId: data.session_id ?? null,
      version: data.v,
      attempts: 0,
      // Estimated rows in the gap ≈ its span in seconds (ring stores ~1/s),
      // bounded by what the ring can actually hold. Display-only denominator.
      totalEst: Math.min(
        Math.max(1, Math.round(data.uptime - autoPrevUptimeS)),
        HISTORY_RING_CAPACITY,
      ),
    });
    // What the app's poller saw while the gap was open: timeouts vs neterrors.
    // "t0 n0" ⇒ the app wasn't polling (backgrounded) — no AppState bookkeeping
    // needed, since a foreground gap always registers failed polls.
    const c = drainFailCounts();
    pendingPollFail = `t${c.t} n${c.n}`;
    void pumpBackfills();
  }

  if (data.uptime != null) autoPrevUptimeS = data.uptime;
}

// Drain the pending-gap queue, oldest first, one fetch at a time. On a fetch
// failure the gap stays at the head of the queue and the pump stops — it's
// retried by the next frame or the next autoTick, so a gap survives an
// arbitrarily long outage and fills the moment the link is usable again.
// Single-flight via pumpRunning so overlapping triggers (choppy WiFi) can't
// race; unlike the old per-fetch guard, queued gaps are never dropped.
async function pumpBackfills(): Promise<void> {
  if (pumpRunning) return;
  pumpRunning = true;
  setBusy('backfill', true);
  try {
    while (pendingGaps.length > 0) {
      if (!unsubscribe) { pendingGaps = []; break; }  // flight ended
      const ip = getCurrentIP();
      if (!ip) break;                                  // no link; retry on next tick
      const g = pendingGaps[0];
      // New attempt at this gap → progress restarts from 0. A prior failed
      // attempt refetched from g.sinceMs, so its rows are being re-pulled, not
      // accumulated; the reset is the truthful "it's looping" signal.
      syncSynced   = 0;
      syncTotalEst = g.totalEst;
      try {
        const { sessionId, records } = await fetchHistorySince(
          ip, g.sinceMs, BACKFILL_MAX_PAGES,
          (received) => { syncSynced = Math.min(received, syncTotalEst); },
        );
        // Boat rebooted across this gap → ring is a different flight, the hole
        // is unrecoverable. Drop it rather than retrying forever.
        if (g.sessionId != null && sessionId !== g.sessionId) {
          console.warn(
            `[telemetryLogger] backfill gap since=${g.sinceMs}ms abandoned — ` +
            `session changed (${g.sessionId}→${sessionId}); ring cleared by reboot`,
          );
          pendingGaps.shift();
          continue;
        }
        if (!unsubscribe) { pendingGaps = []; break; }  // flight ended mid-fetch
        mergeBackfill(records, g.anchorUptimeMs, g.anchorWallMs, g.version);
        pendingGaps.shift();                            // filled — done with this gap
      } catch (e) {
        // Surface the REAL failure cause (timeout/abort/network) with the
        // cursor + attempt count so a recurring failure is diagnosable from the
        // log instead of inferred. Leave the gap queued for retry.
        g.attempts++;
        console.warn(
          `[telemetryLogger] backfill gap since=${g.sinceMs}ms attempt ${g.attempts} failed; ` +
          `${pendingGaps.length} gap(s) pending. cause:`, e,
        );
        break;
      }
    }
  } finally {
    pumpRunning = false;
    setBusy('backfill', false);
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

  let added = 0;
  for (const rec of records) {
    // floor, not round — matches the boat's live uptime (millis()/1000, a floor)
    // so a backfilled second and its live twin share one key and can't both land.
    const sec = Math.floor(rec.uptime_ms / 1000);
    if (seenSeconds.has(sec)) continue;  // already have this second (live or backfilled)
    seenSeconds.add(sec);

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

let lastCheckpointAt = 0;

function autoTick() {
  // Only relevant while a flight is running.
  if (!unsubscribe) return;

  // Crash-safety: mirror the in-progress flight to disk periodically so an app
  // kill/eviction before finalize loses at most CHECKPOINT_MS of data.
  const now = Date.now();
  if (now - lastCheckpointAt >= CHECKPOINT_MS) {
    lastCheckpointAt = now;
    void checkpointDraft();
  }

  // Retry any gap whose /history fetch hasn't succeeded yet. Frame-driven
  // pumping only fires when frames arrive; this guarantees a gap still fills
  // during a quiet stretch once the link comes back.
  if (pendingGaps.length > 0) void pumpBackfills();

  // Boat-gone detection. A short/medium WiFi dropout is bridged by /history
  // backfill, not by ending the flight, so the threshold is deliberately long.
  // Only judge "gone" while foregrounded: a backgrounded app isn't polling, so
  // its silence says nothing about the boat. (Also covers the first tick after
  // resume firing before onAppStateChange — appState still reads non-active.)
  if (appState !== 'active') return;
  // Judge silence against the later of the last real frame and the last
  // foreground bump. This equals the old single-variable value (each event
  // overwrote it with its own timestamp, so the live value was always the most
  // recent of the two) — boat-gone behavior is unchanged; only the display
  // anchor was split out.
  const goneAnchor = Math.max(autoLastFrameAt, autoForegroundedAt);
  if (goneAnchor === 0) return;
  if (now - goneAnchor > AUTO_GONE_THRESHOLD_MS) {
    stop();  // saves the flight
  }
}

// Wire the auto engine to the telemetry stream. Idempotent — safe to
// call multiple times. Call once from App.tsx on mount.
export function initAutoLogger() {
  if (autoInitialized) return;
  autoInitialized = true;
  startHeartbeat();      // JS-thread liveness probe; must run from app mount so a
                         // stall right after connect is caught, not just a mid-run one
  void recoverDraft();   // salvage a flight the app was killed mid-recording
  subscribe(autoOnFrame);
  appState = AppState.currentState ?? 'active';
  appStateSub = AppState.addEventListener('change', onAppStateChange);
  autoTickHandle = setInterval(autoTick, AUTO_TICK_MS);
}

// Test-only / shutdown helper. Not used by the app today but kept so
// the tick interval can be torn down cleanly if needed.
export function shutdownAutoLogger() {
  if (autoTickHandle) { clearInterval(autoTickHandle); autoTickHandle = null; }
  if (appStateSub) { appStateSub.remove(); appStateSub = null; }
  autoInitialized = false;
}
