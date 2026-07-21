// flightlogService.ts
// Client + import flow for the firmware's full-mission flash log (v0.13.0):
//   GET  /flights                      — stored-file inventory + health
//   GET  /flight?name=mN&since_ms=…    — one page, same shape/cursor as /history
//   POST /flight/delete {name}         — after a verified import
//
// A stored boat file becomes a standalone saved flight in the app: records are
// paged with the same cursor loop as the /history backfill (pageSince),
// timestamped from the persisted per-session anchor (true wall clock) or
// relative-to-boot when the app never polled that boot, saved + read back to
// verify, and only then deleted from the boat. Live telemetry and the
// /history backfill path are untouched.

import { Alert } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { HTTP_PORT } from '../constants';
import { HistoryPage, pageSince } from './historyService';
import { getAnchor } from './anchorRegistry';
import { saveImportedFlight, loadFlightRows, FlightMeta } from './telemetryLogger';

export interface BoatFlightFile {
  name: string;         // "m<N>"
  session_id: number;   // boot that recorded it (0 if the header was unreadable)
  records: number;      // whole records on flash (≈ seconds of mission)
  bytes: number;
  active: boolean;      // this boot's growing file — never offered for import
}

export interface BoatFlights {
  enabled: boolean;
  full: boolean;
  active: string;
  free_bytes: number;
  dropped: number;
  files: BoatFlightFile[];
}

const TIMEOUT_MS = 5000;
// An 8 h file is ~288 pages; cap well above that so no real file truncates.
const IMPORT_MAX_PAGES = 400;

const IMPORTED_KEY = 'flightlog:imported:v1';   // session_ids already imported
const IMPORTED_MAX = 50;

async function getJSON<T>(url: string): Promise<T> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
  try {
    const res = await fetch(url, { signal: controller.signal });
    if (!res.ok) throw new Error(`${url.replace(/^http:\/\/[^/]+/, '')} failed: ${res.status}`);
    return (await res.json()) as T;
  } finally {
    clearTimeout(timer);
  }
}

export async function fetchBoatFlights(ip: string): Promise<BoatFlights> {
  return getJSON<BoatFlights>(`http://${ip}:${HTTP_PORT}/flights`);
}

export async function fetchFlightPage(ip: string, name: string, sinceMs: number): Promise<HistoryPage> {
  return getJSON<HistoryPage>(
    `http://${ip}:${HTTP_PORT}/flight?name=${encodeURIComponent(name)}&since_ms=${sinceMs}`,
  );
}

export async function deleteBoatFlight(ip: string, name: string): Promise<void> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
  try {
    const res = await fetch(`http://${ip}:${HTTP_PORT}/flight/delete`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name }),
      signal: controller.signal,
    });
    if (!res.ok) throw new Error(`/flight/delete ${name} failed: ${res.status}`);
  } finally {
    clearTimeout(timer);
  }
}

// ── Imported-session registry ───────────────────────────────────────────────
// Files are deleted from the boat after import, so this mostly matters when
// that delete fails (link dropped right after the save): the file stays on
// the boat but is no longer offered. Keyed by session_id — file NAMES recycle
// (numbering restarts after the flash empties), session ids don't.

async function readImported(): Promise<number[]> {
  try {
    const raw = await AsyncStorage.getItem(IMPORTED_KEY);
    const arr = raw ? (JSON.parse(raw) as number[]) : [];
    return Array.isArray(arr) ? arr : [];
  } catch {
    return [];
  }
}

async function markImported(sessionId: number): Promise<void> {
  if (!sessionId) return;
  const arr = await readImported();
  if (!arr.includes(sessionId)) arr.push(sessionId);
  try {
    await AsyncStorage.setItem(IMPORTED_KEY, JSON.stringify(arr.slice(-IMPORTED_MAX)));
  } catch {}
}

// Non-active, non-empty files from boots not yet imported. session_id 0 means
// the boat couldn't read the file header — offer it anyway (import still
// works; it just can't be tracked across a failed delete).
export async function listRecoverable(ip: string): Promise<BoatFlightFile[]> {
  const info = await fetchBoatFlights(ip);
  const imported = new Set(await readImported());
  return (info.files ?? []).filter(
    (f) => !f.active && f.records > 0 && !(f.session_id !== 0 && imported.has(f.session_id)),
  );
}

// ── Import ──────────────────────────────────────────────────────────────────

export interface ImportProgress {
  received: number;   // records confirmed received so far
  total: number;      // the file's record count from /flights
}

// Pull one stored file end-to-end and save it as its own flight:
//   page /flight → timestamp rows (anchor or relative) → save + read back to
//   verify → mark session imported → delete the boat's copy.
// Throws (and leaves the boat's copy untouched) on any failure before the
// verified save; a failed delete AFTER the save is only warned — the flight
// is safe on the phone and the registry stops it being offered again.
export async function importBoatFlight(
  ip: string,
  file: BoatFlightFile,
  onProgress?: (p: ImportProgress) => void,
): Promise<FlightMeta> {
  const maxPages = Math.min(IMPORT_MAX_PAGES, Math.ceil(file.records / 100) + 2);
  const { sessionId, records, v } = await pageSince(
    (cursor) => fetchFlightPage(ip, file.name, cursor),
    0,
    maxPages,
    (received) => onProgress?.({ received, total: file.records }),
  );
  if (records.length === 0) throw new Error(`${file.name}: no records returned`);

  const sess = file.session_id || sessionId;
  const anchor = await getAnchor(sess);
  const rows: Array<Record<string, unknown>> = records.map((rec) => {
    const { seq, uptime_ms, ...fields } = rec;
    void seq;
    // Anchored: same conversion the /history backfill uses. No anchor: ts =
    // boat uptime_ms — epoch-1970 timestamps that read as T+h:mm:ss, flagged
    // on the flight meta as relative time. `v` (firmware ≥0.13.1) is the
    // version that recorded the file; absent on older firmware → empty cell.
    const ts = anchor ? anchor.wallMs - (anchor.uptimeMs - uptime_ms) : uptime_ms;
    return { ...fields, ts, uptime: Math.floor(uptime_ms / 1000), ...(v ? { v } : {}) };
  });

  const meta = await saveImportedFlight(rows, {
    relative: anchor == null,
    boatFile: file.name,
    boatSessionId: sess,
  });

  // Verified save: the CSV on disk must hold every row before the boat's copy
  // is touched.
  const back = await loadFlightRows(meta.id);
  if (back.length < rows.length) {
    throw new Error(`${file.name}: verify failed (${back.length}/${rows.length} rows on disk) — boat copy kept`);
  }

  await markImported(sess);
  try {
    await deleteBoatFlight(ip, file.name);
  } catch (e) {
    console.warn(`[flightlog] ${file.name} imported but boat-side delete failed (copy remains on boat)`, e);
  }
  return meta;
}

// ── On-connect prompt ───────────────────────────────────────────────────────
// After a connect, if the boat holds unimported mission files, point the
// operator at FLIGHT LOGS. One alert per distinct file-set per app session;
// never blocks the connect flow; errors are swallowed (a pre-v0.13.0 boat has
// no /flights — the FLIGHT LOGS screen is the real surface either way).

let promptedKey = '';

export async function promptRecoverable(ip: string): Promise<void> {
  try {
    const files = await listRecoverable(ip);
    if (files.length === 0) return;
    const key = files.map((f) => `${f.name}:${f.session_id}`).join(',');
    if (key === promptedKey) return;
    promptedKey = key;
    Alert.alert(
      'Mission logs on boat',
      `${files.length} unimported mission log${files.length === 1 ? '' : 's'} from previous runs ` +
      'stored on the boat. Open FLIGHT LOGS to import.',
    );
  } catch {}
}
