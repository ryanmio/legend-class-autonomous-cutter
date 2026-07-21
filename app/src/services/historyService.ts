// historyService.ts
// Client for the firmware's GET /history endpoint (store-and-sync gap backfill).
// The boat keeps a RAM ring of compact per-second records; after a WiFi dropout
// the app pulls whatever it missed so the flight log has no hole.
//
// Records are telemetry-shaped (same field names/types as /telemetry) plus
// `seq` and `uptime_ms`, so the logger can slot them straight into a flight.

import { HTTP_PORT } from '../constants';

export interface HistoryRecord {
  seq: number;
  uptime_ms: number;
  // Remaining keys mirror TelemetryData (mode, lat, lon, heading, port_us, …).
  [key: string]: unknown;
}

export interface HistoryPage {
  session_id: number;
  more: boolean;
  records: HistoryRecord[];
  // /flight only (firmware ≥0.13.1): version that RECORDED the file. Absent
  // on /history pages — live rows already know the running version.
  v?: string;
}

const TIMEOUT_MS = 5000;

export async function fetchHistoryPage(ip: string, sinceMs: number): Promise<HistoryPage> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
  try {
    const res = await fetch(`http://${ip}:${HTTP_PORT}/history?since_ms=${sinceMs}`, {
      signal: controller.signal,
    });
    if (!res.ok) throw new Error(`/history failed: ${res.status}`);
    return (await res.json()) as HistoryPage;
  } finally {
    clearTimeout(timer);
  }
}

// The cursor pager, generic over the page fetcher so the flash-log import
// (flightlogService, GET /flight) reuses the exact same loop as the /history
// backfill: advance since_ms to each record's uptime_ms, stop on an empty
// page, `more:false`, or the page cap.
//
// onPage (optional) reports progress: called after each page is confirmed
// received (the fetch resolved and its records are in hand), with the running
// count of records received so far this call. It is display-only — it does not
// change the return value, and the caller still advances its persistent
// watermark only after this whole call resolves and the records are merged. If
// a page throws, this call rejects and the accumulated `out` is discarded, so
// the reported progress for a failed call is provisional by design.
export async function pageSince(
  fetchPage: (sinceMs: number) => Promise<HistoryPage>,
  sinceMs: number,
  maxPages: number,
  onPage?: (receivedSoFar: number) => void,
): Promise<{ sessionId: number; records: HistoryRecord[]; v?: string }> {
  let cursor = sinceMs;
  let sessionId = 0;
  let v: string | undefined;
  const out: HistoryRecord[] = [];
  for (let page = 0; page < maxPages; page++) {
    const p = await fetchPage(cursor);
    sessionId = p.session_id;
    if (p.v) v = p.v;
    if (!p.records || p.records.length === 0) break;
    for (const r of p.records) {
      out.push(r);
      cursor = r.uptime_ms;
    }
    onPage?.(out.length);   // page confirmed received; report cumulative count
    if (!p.more) break;
  }
  return { sessionId, records: out, v };
}

// Page through every /history record newer than sinceMs. Returns the boat's
// session_id so the caller can refuse to merge across a reboot.
export async function fetchHistorySince(
  ip: string,
  sinceMs: number,
  maxPages = 50,
  onPage?: (receivedSoFar: number) => void,
): Promise<{ sessionId: number; records: HistoryRecord[] }> {
  return pageSince((cursor) => fetchHistoryPage(ip, cursor), sinceMs, maxPages, onPage);
}
