# Telemetry store-and-sync (idea + explored design)

## Problem

Telemetry is stream-only. If WiFi drops but the boat still has radio (RC),
all telemetry for that disconnected window is lost forever — the app only
logs what it receives live.

## Idea

Store telemetry onboard the boat and sync it to the app when the connection
returns. Likely shape: stream while WiFi is up; when WiFi is gone, buffer
onboard; on reconnect, sync the gap so the log/CSV is complete.

## Note

When this is built we likely want to **remove/trim some telemetry fields**.
Several are sent every frame but unused by the app, and a leaner payload
matters more once records are being buffered and synced.

---

# In plain terms (what's new, beginner view)

**The problem this fixes:** if WiFi dropped during a run, that stretch of data
was gone forever, and a long drop split your trip into two separate logs.

**The two changes that fix it:**

1. **The boat now keeps a rolling ~20-minute record of itself** (in memory),
   even when no phone is connected. So the data still *exists* during a dropout
   instead of vanishing.
2. **When the phone reconnects, it asks the boat "what did I miss?"** and stitches
   those readings into the current trip. A WiFi drop no longer ends a trip — only
   a boat restart, a manual Stop, or 5 minutes of total silence does.

**Result:** one continuous log per outing, with WiFi dropouts filled in, instead
of logs full of holes or split in two.

### What "save" means here

"Save" = the **phone** writing the trip to a file in its own storage (the flight
list you can open and export as CSV). It does *not* mean pulling data off the
boat. The boat's onboard record is only a **short-term backup buffer** for
filling gaps; the real, permanent log always lives on the phone.

### The 5-minute trade-off

The phone can't tell "boat powered off" from "WiFi blipped" — both look like
silence. So it waits 5 minutes of silence before declaring a trip finished
(up from the old 60 s). That longer wait is exactly what lets a long WiFi outage
be bridged instead of splitting the trip. The cost: a real end-of-session
auto-saves within 5 min instead of 1 — hit **Stop** in the app for an instant
save. No data is lost either way; it's only about *when* the file gets written.

### Crash-safety

The in-progress trip lives in the phone's memory until it's saved. If the phone
closes the app to free memory before then, that could lose the trip — so while
recording, the phone also mirrors it to a draft file every 30 seconds and
recovers it on next launch. Worst case you lose the last ~30 seconds, not the
whole trip.

### The one honest limitation

The phone's log is only complete if it **reconnects before you power off** — the
gap-fill happens on reconnect. If WiFi drops *and* you immediately kill the
boat's power without ever reconnecting, that last gap is lost (the boat's backup
buffer dies with the power). In normal use — WiFi comes back while you're still
running — everything gets filled in.

---

# Explored design (2026-06-15)

## Decision

RAM ring buffer is the chosen approach (agreed 2026-06-15). The central change
is **removing the app's 60 s-of-silence flight-split rule** — a WiFi dropout no
longer ends a flight; only a boat reboot or a genuinely-gone boat does. We are
not working around the split; we are deleting the rule that causes it. Onboard
flash/SD persistence is explicitly out of scope for now (see rationale below).

## What's actually there today

- **Transport is HTTP polling**, not WebSocket. `app/.../websocketService.ts`
  polls `GET /telemetry` every 1 s; the name is historical.
- **Firmware stores nothing.** `/telemetry` (`firmware/legend_cutter/telemetry.cpp`)
  serializes the *current instant* on each GET. No SD, no SPIFFS/LittleFS, no
  RAM ring. NVS is used only for mag cal.
- **No wall clock onboard.** Firmware has `millis()`/`uptime` only. Each logged
  row's `ts` is stamped by the **app** with `Date.now()` on receipt
  (`telemetryLogger.ts`).
- **The app already ends a flight on two events** (`telemetryLogger.ts` auto
  engine): `session_id` change (boat rebooted) and 60 s of telemetry loss
  (`AUTO_LOST_THRESHOLD_MS`). A flight auto-starts on throttle-up.

## The real problem is two problems

1. **No gap data exists** to sync — the boat keeps nothing while WiFi is down.
2. **Even if it did, the app would split the flight.** A WiFi drop >60 s makes
   the auto-logger `stop()` (save partial flight), then a reconnect starts a
   *new* flight. So store-and-sync is a coordinated firmware **and** app change,
   not just "add a buffer." This is the part that's easy to miss.

## Recommended approach: onboard RAM ring + `/history` cursor + reconnect-aware app

### Why RAM, not flash/SD

The failure mode in the idea is **WiFi drop while the boat is still powered and
running** — RAM survives that perfectly. The only thing a RAM ring loses is a
**reboot/brownout**, and the app *already* treats a reboot (`session_id` change)
as a hard flight boundary — so a RAM ring loses nothing the system currently
keeps anyway. Flash/LittleFS would add reboot-survival at the cost of wear
management, a partition change, and more code, for a case the product already
defines as "flight over." There is no SD reader on the sealed boat. So: RAM ring
now; LittleFS is a clean later upgrade if reboot-survival is ever wanted.

### Sizing

A compact binary record (not the fat JSON) is ~36 bytes:

| field | bytes | note |
|-------|-------|------|
| seq (uint32) | 4 | monotonic per boot; the sync cursor + dedup key |
| uptime_ms (uint32) | 4 | app maps to wall clock via the live anchor |
| lat,lon (int32 ×2, deg×1e7) | 8 | |
| heading,course,speed (int16 ×3) | 6 | speed = kts×100 |
| esc_us,rudder_us (int16 ×2) | 4 | |
| batt_cv,batt_ca (int16 ×2) | 4 | centivolts / centiamps |
| depth_cm,wp_dist_m (int16 ×2) | 4 | |
| mode,sats,flags,_pad (uint8 ×4) | 4 | flags = gps_fix/wp_set/captured/pump… |

At 1 Hz: ~2.2 KB/min, ~130 KB/hr. A **1200-record ring (~43 KB, ~20 min)**
covers any realistic pool WiFi gap with margin, declared as a static `.bss`
array so the footprint is fixed at link time (not heap, which WiFi+server churn).
All sizes in `config.h` (`HISTLOG_CAPACITY`, `HISTLOG_INTERVAL_MS`).

### Firmware change (new `histlog.cpp/.h`, ~1 module)

- Static ring of the record above. `histlogUpdate()` appends the current state
  every `HISTLOG_INTERVAL_MS` (~1 s), called from the main loop. Independent of
  WiFi state — it always records, so there's no "did we notice the drop" race.
- New endpoint `GET /history?since=<seq>` → returns records with `seq > since`
  as compact CSV text (smaller + streamable than JSON), capped per response
  (e.g. 300 rows); the app pages with the cursor until caught up. Response
  header/first line carries `session_id` so the app can detect a reboot mid-sync.
- **Zero behavior change by default** (per the bench-rig rule): the ring just
  fills in the background; live telemetry, control, and the `/telemetry` wire
  contract are untouched. Bump `FIRMWARE_VERSION` → `0.5.0` (MINOR, new feature).

### App change (`telemetryLogger.ts` auto engine)

- **Make the flight boundary reboot-driven, not gap-driven.** A WiFi drop no
  longer finalizes a flight as long as the boat comes back with the *same*
  `session_id`. Finalize on `session_id` change, on a long *unrecovered* loss
  (boat truly gone), or manual stop. Raise/replace `AUTO_LOST_THRESHOLD_MS`
  with a "gone, not just blipped" timeout.
- **On reconnect, pull the gap.** When polling resumes and `session_id` matches,
  call `/history?since=<lastSeqSeen>`, page to the end, and merge records into
  the running flight. Convert each record's `uptime_ms` → wall clock using the
  live anchor the app already has on every frame (`uptime` + local `Date.now()`):
  `ts = now_wall - (uptime_now - uptime_rec)`. Dedup/sort by `seq`.
- Live 1 Hz polling stays the primary path; `/history` only backfills holes.
  `seq` makes interleaving live + backfilled rows idempotent.

### On the field-trim note

Keep it **out of the critical path.** The lean schema lives naturally in the new
binary record above — that's where "trim unused fields" actually pays off. Do
**not** trim the live `/telemetry` JSON as part of this: it's a frozen wire
contract (parity-checked vs the test_29 oracle, near its 2 KB cap), and trimming
it is a separate, app-breaking change with its own risk. Two schemas, decoupled.

## Status — IMPLEMENTED (branch `telemetry-store-and-sync`)

Both halves are built and verified. Where the implementation differs from the
sketch above: the `/history` query is by **`since_ms` (record uptime)**, not
`since=<seq>` — the app already knows the boat's `uptime` from every live frame
but never sees a `seq`, so uptime is the natural cursor (`seq` is still in each
record for ordering). Records are returned as JSON objects with the same field
names as `/telemetry`, so they merge straight into a flight. Dedup is by
**second**, not `seq`, for the same reason (live rows carry uptime, not seq).

**Firmware (v0.5.0)** — new `histlog` module: a 1200-record RAM ring (~20 min)
of compact per-second samples, recorded every loop regardless of WiFi.
`GET /history?since_ms=<uptimeMs>` serves the gap, oldest-first, paged at 100
records (chunked send, no large buffer). Live `/telemetry` untouched;
`parity_check` shows only the new `/history` route. `arduino-cli compile`:
globals 30% of RAM, 229 KB free.

**App** — `historyService.ts` pages `/history`; `telemetryLogger.ts` auto engine
reworked: the 60 s flight-split is gone. A flight ends only on a reboot
(`session_id` change), a 5 min unrecovered absence (`AUTO_GONE_THRESHOLD_MS`),
or manual stop. On reconnect, a jump in the boat's `uptime` between frames
triggers a backfill that merges the missing records into the running flight
(ts derived from the live anchor frame). `tsc` clean (one unrelated pre-existing
error in `HelmScreen.tsx` left as-is).

The 5 min absence timeout is the accepted trade-off: a real end-of-session
(boat powered off) auto-saves within 5 min instead of the old 60 s — use the
app's manual stop for an immediate save. That longer timeout is exactly what
lets a long WiFi outage be bridged instead of splitting the flight.

**Crash-safety (draft checkpoint).** The in-progress flight lives in phone RAM
until it's finalized to a file. If the OS evicts the app before then (likely
over hours, certain over days), that flight would be lost — and the longer 5 min
timeout widened that window. So while recording, the flight is mirrored to a
`_draft.csv` on disk every 30 s (on the timer that already runs); on next launch
any leftover draft is recovered into a real saved flight and removed. Worst-case
loss is the last ~30 s, not the whole flight. Reuses the existing file/CSV/stats
helpers — no new dependencies. (No AppState background-save hook; the periodic
checkpoint covers it with far less complexity.)

## Recommendation (original plan — followed)

Build it, in this order, but as **two shippable steps**:

1. **Firmware ring + `/history`** (0.5.0). Self-contained, no behavior change,
   independently testable ("bench the boat": drive it, pull `/history`, eyeball
   the CSV). Low risk.
2. **App reconnect-aware logging + backfill.** The higher-judgement half — it
   rewrites the auto-logger flight-boundary FSM, so it deserves its own review
   and a deliberate "WiFi off mid-run, then back" bench test.

Net: ~1 firmware module + 1 endpoint + a focused rewrite of the app auto engine.
The architecture (HTTP polling, `session_id`, app-side `ts`, per-flight CSV) all
already supports this; nothing structural has to change.

### Deferred / not now

- LittleFS persistence (reboot-survival) — only if a future need appears.
- WebSocket/SSE push to replace polling — orthogonal; polling + `/history` is
  enough and simpler.
- Trimming the live `/telemetry` contract — separate effort if/when wanted.
