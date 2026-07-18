# Task A — reconnect stall / multi-minute lockout: open items

**Status: CLOSED (2026-07-18).** Root cause reproduced on the bench, fix committed
(`be07515d`), and the outstanding field confirmation is now done — see "Field
confirmation" below. Full narrative is in `RECONNECT_STALL_REPORT.md`.

**History (resolved):** paused ~2 weeks from 2026-07-13 after the 4S pack
over-discharged (cell 4 at 1.12 V, scrapped) — the hull had to be cut to swap it.
Battery replaced; testing resumed and closed the item 2026-07-18.

---

## Root cause (reproduced on the real boat)

A large pending `/history` backfill backlog **plus** a marginal link makes
`pumpBackfills` retry a full multi-page pull every 5 s that never completes. On a
marginal link the pages keep failing, so each retry restarts from `since_ms` and
re-pulls — connection churn at the boat's **single-client** web server that
**scales with backlog size** and does **not** stop when the poll link is fine (it
only stops when the backfill *completes*). That churn jams the boat **for everyone**.
Self-sustaining; **duration, not distance** — the more backlog, the longer the lockout.

Bench reproduction (`scratchpad/repro_backlog2.py`, real boat + real `/history`,
clean *direct* witness measuring the boat's health for everyone):

| condition (whole-ring backlog) | clean witness | backfill |
|---|---|---|
| **unfixed** + marginal 60% link | **36–39% — LOCKED OUT** | 21–25 attempts, never drains |
| **fixed** (6.2) + marginal 60% link | **83%** | 9 attempts |
| control: **good** link (no marginal) | **100%** | drains in **2 s** |

The control is the key: **backlog alone doesn't jam, a marginal link alone doesn't
jam — both together do.** Once the churn jams the boat it self-sustains even at full
signal, because the pages then fail *because of* the churn. No reboot in any run
(session id stable throughout — no crash).

## Fix — committed as `be07515d`

- **6.2 (the one that breaks the loop):** gate `pumpBackfills` on `isConnected()` in
  `telemetryLogger.ts` — don't pull `/history` while the live poll link is unhealthy.
  `break`, not `shift`, so the gap stays queued and fills once the link is back.
- **6.1 (helps, secondary):** poll-loop exponential backoff in `websocketService.ts`
  (snap reset to 1 Hz on first success, 8 s cap). Throttles poll-loop churn; not the
  thing that breaks the backfill loop.
- **nconsec3** (stickier backoff) was evaluated and **not adopted** — it's about
  poll-backoff stickiness, irrelevant to this backfill mechanism.

## Field confirmation — DONE (2026-07-18), PASS

Field excursion on the fixed build, end-to-end through the real phone/hotspot stack
(the bench can't exercise it — it uses a Mac client + a software loss proxy).

- **Void first attempt** (`mission_logs/debug/2026-07-18T15-12-07.csv`): the phone
  backgrounded and iOS suspended the JS engine for the whole ~24.6-min gap
  (`js_freeze late=1477600 at=15:20:15`). A suspended app makes no calls, so it never
  generated the churn the fix targets — a clean reconnect there proves nothing.
  Lesson baked into the re-run: **Auto-Lock → Never, app foreground, screen on**, and
  validate the run by checking the reconnect row's `js_freeze` is small.
- **Valid pass** (`mission_logs/debug/2026-07-18T19-17-00.csv`): app awake throughout
  (`js_freeze` only at startup). A real **9.3-min** out-of-range excursion (uptime
  442→1001; reconnect marker `t39 n23` — the neterrors confirm *genuinely out of range*,
  not a jam). Result: **no multi-minute lockout, no `session_id` change** (session
  `2043089097` stable), and the whole gap **backfilled seamlessly** (559/560 s at 1 s).
  Reconnect took ~30–60 s, fully accounted for by the backoff (8 s cap) spacing retries
  while walking back through a marginal-signal zone — intended tradeoff, not a jam.
  Reconnect time is bounded by that cap; it does **not** grow with outage length.

Closure rests on **bench-proves-causality** (`repro_backlog2.py`: unfixed 36–39% locked
out vs fixed 83%) **+ field-confirms-behavior** (above). The field never independently
reproduced the jam because its trigger (foreground + big backlog + sustained marginal
link) is hard to hit outside a controlled loss proxy — not a gap in the fix.

## Honest residual — do NOT record as fully solved under bad conditions

At 60% simulated loss, fixed is **83%, not 100%**. That remaining gap is the
*operator's own* marginal link, not the boat being jammed. The fix converts
"**boat locked out for everyone**" into "**operator's own link degraded, boat still
reachable**." That is the correct behavior — but under genuinely bad link conditions
the operator's telemetry is still degraded, and the backfill may still fail to
complete until the link improves. Don't overstate it as "solved under all conditions."

## Known separate, still OPEN — do not fold into the jam theory

The **home-WiFi bench stalls**: a Mac probe got **464/464** polls while the app went
**blind for 234 s on a perfect link**, including a **45 s "zombie fetch"** where the
JS thread was healthy (heartbeat fine) yet a `fetch` neither resolved nor fired its
abort. This is a **separate, unexplained** app-side failure, NOT the backfill jam.
The shipped **`js_freeze` and `poll_fail`** instruments will classify it on a future
run (they stamp JS-thread freeze magnitude/AppState and per-gap poll-failure counts
into the flight CSV). Left open.

## Cancelled permanently — do NOT revive

The **firmware WiFi retry-cadence / periodic re-scan change.** The boat's WiFi
association was exonerated three independent ways (`wifi_assoc=true` throughout,
zero neterrors across ~32 min of stalls, and the residual test showing the boat
built every recovery response fresh). It was never the boat's association. Do not
bring this change back.

## Not this bug

The original **Potomac failsafe event was genuine range** (boat RSSI ≈ **−95 dBm**):
the boat went out of range, hit failsafe, and stopped — the system **working
correctly**. That is distinct from the reconnect lockout diagnosed here.

---

## Logging / CSV fields — keep all for now (revisit note)

Question raised before the Potomac run: cut diagnostic CSV fields to save space / sync
faster? **Decided: keep all.** Cutting CSV fields does **not** speed sync — the two are
different payloads. The sync payload (`GET /history`, the backfill) is a fixed compact
`HistRecord` (~21 nav fields, ~50 bytes/s; see `firmware/legend_cutter/histlog.h`) that
carries none of the wide CSV columns and none of the app-computed diagnostics
(`js_freeze`, `poll_fail` are stamped on the phone, never sent). CSV width is a
*local phone-storage* cost only (~800 KB/run, dominated by the boat's live `/telemetry`
fields, not the 2 diagnostic columns). The real sync lever — one-time 2 s coarsening on
>20 min gaps — already shipped (v0.9.0) and proved out on the 24.6-min gap above.

**Revisit only if:** (a) local phone storage becomes a real constraint on long runs —
then trim the wide boat-state columns (`mag_cal_*`, `radar_*`, `pump_*`), NOT the
diagnostics; or (b) the home-WiFi zombie-fetch bug (above) is closed — then
`js_freeze`/`poll_fail` can retire. To shrink *sync* itself you'd cut fields from
`HistRecord`, but every one is load-bearing for track + multi-waypoint playback + health.

## Test harnesses (in `scratchpad/`, not committed — recreate from the report if needed)

- `repro_backlog2.py` — the decisive one: backlog + marginal link + clean witness.
- `repro_backlog.py` — backlog on a good link (the 2 s-drain control).
- `repro_lockout.py` — flutter without a backlog (why the earlier "doesn't reproduce"
  was a harness gap: no backlog → poll loop is the only churn → quiesces on good link).
- `boat_probe.py` — dual independent HTTP witness (fresh + keepalive).
- `churn_test.py` / `control.py` — the (separate) half-open-orphan jam + clean-close control.
