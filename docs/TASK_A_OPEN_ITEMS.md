# Task A — reconnect stall / multi-minute lockout: open items

**Status: PROVISIONALLY CLOSED.** Root cause reproduced, fix committed
(`be07515d`), one field confirmation outstanding.

**Project hiatus (~2 weeks from 2026-07-13):** the boat's 4S pack was over-discharged
(cell 4 at 1.12 V, unrecoverable, scrapped). Replacing it requires cutting the hull.
No bench or field testing is possible until that's done. This doc is the pickup point
for whoever resumes — including future-me. Full narrative is in
`RECONNECT_STALL_REPORT.md`; this is the short, actionable version.

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

## THE ONE OUTSTANDING TEST — do this FIRST when the boat sails again

**Field excursion on the fixed build.** Run the boat disconnected long enough to
build a **7+ minute** `/history` backlog (matching the Potomac case), then return to
**close range** and confirm:

1. **Prompt sync** (no multi-minute blackout),
2. **No multi-minute lockout**,
3. **No `session_id` change** (no reboot).

This is the end-to-end proof through the real phone/hotspot stack, which the bench
**cannot** exercise (the bench uses a Mac client + a software loss proxy). **Until
this passes, Task A is provisional.**

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

## Test harnesses (in `scratchpad/`, not committed — recreate from the report if needed)

- `repro_backlog2.py` — the decisive one: backlog + marginal link + clean witness.
- `repro_backlog.py` — backlog on a good link (the 2 s-drain control).
- `repro_lockout.py` — flutter without a backlog (why the earlier "doesn't reproduce"
  was a harness gap: no backlog → poll loop is the only churn → quiesces on good link).
- `boat_probe.py` — dual independent HTTP witness (fresh + keepalive).
- `churn_test.py` / `control.py` — the (separate) half-open-orphan jam + clean-close control.
