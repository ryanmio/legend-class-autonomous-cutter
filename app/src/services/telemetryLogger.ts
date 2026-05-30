// telemetryLogger.ts
// In-memory ring-buffer logger + automatic per-flight save.
//
// Two layers:
//   1. Recorder — start/stop/clear, in-memory rows, CSV builder. Same
//      public API as before; TelemetryScreen still controls it manually.
//   2. Auto engine (initAutoLogger) — always-on subscriber that
//      auto-starts the recorder when the boat is throttled up, and
//      auto-saves the captured rows as a "flight" in AsyncStorage when
//      the boat goes away (telemetry lost / boat rebooted) or the
//      operator manually stops.
//
// Persistence lives entirely on the phone via AsyncStorage:
//   flight:<isoTimestamp>  → CSV body
//   flights:index          → JSON array of { id, startTs, endTs, rowCount }
//
// See FLIGHT_LOG_PLAN.md for the design.

import { Share } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { TelemetryData } from '../types';
import { subscribe } from './websocketService';

export interface LogRow extends TelemetryData {
  ts: number;             // ms since epoch when the frame was received
}

export interface FlightMeta {
  id: string;             // ISO-ish timestamp, also the AsyncStorage key suffix
  startTs: number;        // ms epoch of first row
  endTs: number;          // ms epoch of last row
  rowCount: number;
}

const FLIGHTS_INDEX_KEY = 'flights:index';
const FLIGHT_KEY_PREFIX = 'flight:';

// Cap at 7200 rows ≈ 2 hours @ 1 Hz. Drops oldest when full.
const MAX_ROWS = 7200;

// Auto-engine tuning.
const AUTO_ESC_DEADBAND_US   = 30;        // |esc_us - 1500| must exceed this
const AUTO_ESC_DEBOUNCE_MS   = 500;       // ...for this long to auto-start
const AUTO_LOST_THRESHOLD_MS = 60_000;    // 60 s of no frames = boat is gone
const AUTO_TICK_MS           = 5_000;     // how often to check for telemetry loss

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

// ── CSV building ───────────────────────────────────────────────────────────

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

// Builds CSV from the current in-memory buffer — kept for backward
// compatibility with code that calls getCSV() directly. New code
// should prefer the saved-flight flow (saveFlight / loadFlight).
export function getCSV(): string {
  return rowsToCSV(rows);
}

// Pop a Share sheet with the current in-memory buffer as a string
// message. Same behavior as before. Used by TelemetryScreen's EXPORT
// button. Saved flights have their own export path in FlightsScreen.
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

// ── Saved flights (AsyncStorage) ───────────────────────────────────────────

export async function listFlights(): Promise<FlightMeta[]> {
  const raw = await AsyncStorage.getItem(FLIGHTS_INDEX_KEY);
  if (!raw) return [];
  try {
    const arr = JSON.parse(raw) as FlightMeta[];
    return Array.isArray(arr) ? arr : [];
  } catch {
    return [];
  }
}

export async function loadFlightCSV(id: string): Promise<string | null> {
  return AsyncStorage.getItem(FLIGHT_KEY_PREFIX + id);
}

export async function deleteFlight(id: string): Promise<void> {
  await AsyncStorage.removeItem(FLIGHT_KEY_PREFIX + id);
  const index = await listFlights();
  const next = index.filter((m) => m.id !== id);
  await AsyncStorage.setItem(FLIGHTS_INDEX_KEY, JSON.stringify(next));
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

  await AsyncStorage.setItem(FLIGHT_KEY_PREFIX + id, csv);
  const index = await listFlights();
  index.push({ id, startTs, endTs, rowCount: snapshot.length });
  await AsyncStorage.setItem(FLIGHTS_INDEX_KEY, JSON.stringify(index));
}

// ── Auto engine ────────────────────────────────────────────────────────────

let autoInitialized      = false;
let autoLastSessionId: number | null = null;
let autoLastFrameAt      = 0;
let autoEscAboveSince: number | null = null;
let autoTickHandle: ReturnType<typeof setInterval> | null = null;

function autoOnFrame(data: TelemetryData) {
  const now = Date.now();
  autoLastFrameAt = now;

  // 1. Session-change detection (boat rebooted while we stayed connected).
  if (data.session_id != null) {
    if (autoLastSessionId != null && data.session_id !== autoLastSessionId) {
      if (unsubscribe) stop();  // saves current flight, halts recorder
    }
    autoLastSessionId = data.session_id;
  }

  // 2. Throttle-up detection — ESC commanded out of neutral for the
  //    debounce window. Uses the boat's commanded ESC µs (not raw CH3)
  //    so MANUAL forward, MANUAL reverse-via-right-stick, and AUTO
  //    cruise all trigger uniformly.
  if (data.esc_us != null && Math.abs(data.esc_us - 1500) > AUTO_ESC_DEADBAND_US) {
    if (autoEscAboveSince == null) autoEscAboveSince = now;
    if (!unsubscribe && (now - autoEscAboveSince) >= AUTO_ESC_DEBOUNCE_MS) {
      clear();   // ensure clean buffer for the new flight
      start();
    }
  } else {
    autoEscAboveSince = null;
  }
}

function autoTick() {
  // Telemetry-loss detection. Only relevant while a flight is running.
  if (!unsubscribe) return;
  if (autoLastFrameAt === 0) return;
  if (Date.now() - autoLastFrameAt > AUTO_LOST_THRESHOLD_MS) {
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
