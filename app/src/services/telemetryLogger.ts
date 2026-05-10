// telemetryLogger.ts
// In-memory ring-buffer logger. Subscribes to the telemetry stream and
// records every frame with a timestamp. CSV export via React Native's
// built-in Share sheet. No persistence — operator should export before
// closing the app. (Persistence + file-based export is a follow-up; the
// pool test only needs to capture one ~30 min run.)
//
// Recording is opt-in: nothing starts until start() is called. Screens
// show a START/STOP control on TelemetryScreen and a running-state pill
// on HelmScreen via subscribeRunning().

import { Share } from 'react-native';
import { TelemetryData } from '../types';
import { subscribe } from './websocketService';

export interface LogRow extends TelemetryData {
  ts: number;             // ms since epoch when the frame was received
}

// Cap at 7200 rows ≈ 2 hours @ 1 Hz. Drops oldest when full.
const MAX_ROWS = 7200;

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

export function stop() {
  if (!unsubscribe) return;
  unsubscribe();
  unsubscribe = null;
  notifyRunning();
}

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

// Build the union of column keys across all rows, in stable order. ts +
// v come first; remaining columns alphabetised so the CSV is diff-able
// across runs.
function columns(): string[] {
  const seen = new Set<string>();
  for (const r of rows) for (const k of Object.keys(r)) seen.add(k);
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

export function getCSV(): string {
  if (rows.length === 0) return '';
  const cols = columns();
  const header = cols.join(',');
  const body = rows.map((r) =>
    cols.map((c) => {
      if (c === 'ts') return new Date(r.ts).toISOString();
      return escapeCell((r as unknown as Record<string, unknown>)[c]);
    }).join(',')
  );
  return [header, ...body].join('\n');
}

// Pop a Share sheet with the CSV as a string message. iOS can email/save
// /AirDrop the resulting text. For very large logs (~ tens of MB) this
// may hit platform limits — current cap (~7200 rows × ~30 cols × ~10
// chars ≈ 2 MB) is well within tested ranges.
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
