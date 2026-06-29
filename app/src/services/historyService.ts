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

// Page through every record newer than sinceMs (firmware caps each page; we
// follow the cursor until `more` is false). Returns the boat's session_id so
// the caller can refuse to merge across a reboot.
export async function fetchHistorySince(
  ip: string,
  sinceMs: number,
  maxPages = 50,
): Promise<{ sessionId: number; records: HistoryRecord[] }> {
  let cursor = sinceMs;
  let sessionId = 0;
  const out: HistoryRecord[] = [];
  for (let page = 0; page < maxPages; page++) {
    const p = await fetchHistoryPage(ip, cursor);
    sessionId = p.session_id;
    if (!p.records || p.records.length === 0) break;
    for (const r of p.records) {
      out.push(r);
      cursor = r.uptime_ms;
    }
    if (!p.more) break;
  }
  return { sessionId, records: out };
}
