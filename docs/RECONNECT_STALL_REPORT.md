# Reconnect-stall investigation — final report

**Date:** 2026-07-12 (updated 2026-07-13, closed 2026-07-18)
**Status:** CLOSED (2026-07-18). Root cause of the multi-minute lockout found and
reproduced on the real boat; fix `be07515d` (specifically its 6.2 half) confirmed on
the bench, and confirmed in the field 2026-07-18 — a valid awake run over a 9.3-min
out-of-range excursion showed no lockout, no reboot, and a seamless backfill
(`mission_logs/debug/2026-07-18T19-17-00.csv`). Closure rests on bench-proves-causality
+ field-confirms-behavior; the field never independently reproduced the jam (its trigger
is hard to hit outside a loss proxy), which is not a gap in the fix.
**Details + the void-first-attempt lesson: `docs/TASK_A_OPEN_ITEMS.md`.**
**One line:** A large pending `/history` backfill backlog + a marginal link makes
`pumpBackfills` retry a full multi-page pull every 5 s that never completes, churning
connections that jam the boat's single-client server FOR EVERYONE — a self-sustaining
lockout that scales with backlog size (duration, not distance). Gating the backfill
on link health (6.2) breaks it.

> **REPRODUCED (2026-07-12, `scratchpad/repro_backlog2.py`, real boat + real
> /history, clean direct witness).** Operator field evidence (Potomac t+26:01→30:16:
> standing still 10–15 ft away, 4m15s blackout, following ~7 min disconnected =>
> large backlog; then synced and held the link at 4× distance) overturned the earlier
> "lockout doesn't reproduce" conclusion. My earlier lockout harness had NO backfill
> backlog, so the only churn was the poll loop, which quiesces on a good link — that
> is why I wrongly concluded the lockout couldn't build. Adding the backlog reproduces
> it: whole-ring backlog + marginal 60% link, **unfixed** drives the clean full-signal
> witness to **36–39% (LOCKED OUT)** with the backfill retrying 21–25× and never
> draining; **fixed (6.2 gates the pump on isConnected())** holds the witness at
> **83%** with 9 attempts. On a GOOD link the same backfill drains in 2 s (100%
> witness) — so the marginal link is the required trigger; once the churn jams the
> boat it self-sustains even at full signal because the pages then fail *because of*
> the churn. No reboot in any run (session stable — no crash). The half-open-orphan
> jam (below) is a real but SEPARATE, during-outage-only effect; the sustained lockout
> is the backfill loop. nconsec3 is irrelevant to this mechanism (it was about
> poll-backoff stickiness); snap kept.

---

## 1. Executive summary

The boat's HTTP server (`WebServer`) serves **one client at a time**, has a **listen backlog of 4**, and once it accepts a connection it blocks up to **5 seconds** (`HTTP_MAX_DATA_WAIT`) waiting for a complete request. When the phone's link degrades, the app aborts each poll at 2 s and abandons the socket — **and the abort's teardown packet must cross the same bad link, so it is lost too.** The boat is left holding a half-open socket and blocks on it for 5 s. The app opens a fresh connection ~2 s later (no backoff). Orphans accrue faster than the boat sheds them, so the boat stops answering **anyone** — even a client standing next to it at full signal — until the connection churn stops.

This is the first mechanism that explains the founding observation of the whole investigation: **the stall tracks the _duration_ of the disconnect, not the distance.** Longer outage → more banked-up half-open sockets → longer lockout afterward.

Confidence: **high.** Reproduced on demand at the bench on a perfect link, with a pre-registered control that isolates the single causal variable.

---

## 2. The mechanism, precisely

| Firmware fact | Source | Consequence |
|---|---|---|
| One client served at a time | `WebServer` `_currentClient` | Any single stuck connection blocks all others |
| Listen backlog = 4 | `NetworkServer(max_clients=4)` | Only 4 pending connections before refusal |
| 5 s wait for a full request | `HTTP_MAX_DATA_WAIT = 5000` | One half-open socket costs 5 s of total server availability |
| lwIP TCP PCB pool = 16 | `CONFIG_LWIP_MAX_ACTIVE_TCP=16` | Enough abandoned sockets exhaust the pool → hard refusal / crash |

The app side supplies the churn:

- **Churn engine #1 — the poll loop** (`websocketService.poll`). Opens a **new TCP connection per poll**, re-chained every ~1 s (or immediately after a 2 s timeout). **No backoff.** During an outage this manufactures a new socket every ~2 s ≈ **~30/min**.
- **Churn engine #2 — the `/history` backfill** (`telemetryLogger.pumpBackfills`). Once a gap exists, it retries every `AUTO_TICK_MS` (5 s) **for as long as the gap is unfilled, including while the live link is down** (it only checks `getCurrentIP()`, which stays set during an outage — not link health). Each retry opens another connection with a 5 s timeout. This is the "syncing (0 of 5)" the operator saw flapping past: a backfill failing and restarting, not progressing.

On a **good** link the teardown is delivered, the boat sheds the socket immediately, and the churn is harmless. On a **bad** link the teardown is lost, the socket becomes an orphan, and the churn is lethal.

---

## 3. Evidence

### 3.1 Bench cross-correlation (home WiFi, 2026-07-12 morning)
A second, independent HTTP client (`scratchpad/boat_probe.py`, two sub-clients: `fresh` per-request connections and `keepalive` pooled) polled the same boat at the same instants as the app.
- Across the app's six telemetry gaps — **234 s of app blindness** — the Mac clients got **464 / 464 polls successfully, zero failures.** The boat answered every second the app was blind.
- Conclusion at the time: those home-WiFi stalls were **phone-side**. (See §5 — they may be a distinct bug from the lockout; not unified without evidence.)

### 3.2 Field run on the hotspot (2026-07-12 evening, mission config)
Phone on cellular hosting Personal Hotspot; boat + Mac joined it. Walked the boat to the edge of range and back.
- Edge-of-range stalls: boat's own RSSI hit **−85 to −95 dBm**; both Mac clients died alongside the app in the same seconds. **This was range, not a bug** — the original Potomac failsafe was the system working correctly.
- `wifi_assoc` read **`true` at −95 dBm** with nothing getting through — confirming **`wifi_assoc` is not a liveness signal.**
- The close-range flapping the operator saw was **contaminated by a probe bug** (a scheduler catch-up defect fired the probe at 6–8 req/s instead of 2, itself starving the app). That observation was discarded and the probe fixed.

### 3.3 The decisive experiment (bench, −49 dBm perfect link)
`scratchpad/churn_test.py` + `control.py` + `drain_test.py`:

| Condition | Result |
|---|---|
| Baseline, no churn | **45 / 45 OK**, ~50 ms |
| Leak one half-open socket every 2 s | **Jammed 2 s after churn began** (1 ok / 29 fail) |
| **Control:** aborts that **close cleanly**, same 2 s cadence | **45 / 45 OK — no jam** |
| v2, per-poll `session_id`/`heap`, 10 leaked sockets | **Jammed 2 s in, NO REBOOT (session stable), never recovered in 60 s** |

The **only** variable between "jam" and "no jam" is whether the connection teardown reached the boat. **Every failure was a 2 s TIMEOUT — zero resets, zero refusals.** That is the same "always hangs, never resets" signature seen in every app log; it is not evidence of a healthy boat, it is the fingerprint of a boat jammed by abandoned sockets.

---

## 4. Findings, including corrections

1. **Root cause: lost TCP teardowns → half-open sockets → single-client server jam.** Reproduced on demand.
2. **The jam is not a reboot.** The v2 test held `session_id` constant through baseline, churn, and jam.
3. **Correction (retracted):** an earlier "recovered 3 s after churn stopped" reading was wrong — the boat had **rebooted** 48 s into that churn phase (the operator noticed the power-cycle; confirmed by a `session_id` change). The "recovery" was the ~15 s ESP32 boot, not the jam draining. This retraction removes the earlier claim that a multi-minute field lockout _requires_ a sustained churn engine — the jam alone persists past 60 s.
4. **New severity finding: sustained leaking crashes/reboots the boat.** ~24 half-open sockets in ~48 s took the ESP32 down (lwIP/heap exhaustion). On the vehicle that wipes the RAM history ring, restarts the FSM, and re-inits outputs. **Bounded by:** all 25 field flight logs show one `session_id` each — **the app has never crashed the boat in the field.** The synthetic never-close leak is harsher than the app's real behavior, so the app's churn sits below the crash threshold — but the fragility is real.
5. **Not cleanly measured: self-drain time.** Every test confounded it (reboot / sockets held open / the boat entering the next test still PCB-exhausted from the prior one). Practical note: the boat needs minutes and/or a power-cycle to shed exhausted PCBs between socket-stress tests.
6. **The "zero neterrors" invariant was read backwards early on.** A 2 s abort masks every slow failure (a lost route or dead peer never returns an error inside 2 s), so "never a reset" only means "the boat never actively refused" — a far weaker statement than "the boat was reachable." The bench conclusion still stands because the Mac probe proved reachability independently.

### What this explains
- **Duration-not-distance:** longer outage → more banked half-open sockets → longer post-reconnect lockout.
- **"5 min disconnected, 10 ft away, nothing, then sudden reconnect + backfill":** the boat was jammed by the app's own accumulated orphans; it cleared when the churn finally stopped.
- **Operator's no-recording observation:** walking out of range without recording (→ no `/history` backfill, churn engine #2 silent) produced no flapping. Consistent with backfill being a compounding churn source.

---

## 5. What is NOT claimed

- The **home-WiFi bench stalls (§3.1)** were proven phone-side but may be a **separate** bug from the lockout (e.g. an occasional non-cancelling `fetch`). They are not folded into this root cause without evidence.
- The exact **self-drain time** of a jam is unmeasured.
- Whether the app's _real_ churn (as opposed to the synthetic leak) is by itself enough to sustain a multi-minute field lockout is inferred, not directly reproduced with the real client.

---

## 6. Solution — scoped, simplest first

Design goal: **stop the app from manufacturing connections at a fragile single-client server during exactly the moments the link can't deliver teardowns.** Prefer removing behavior over adding cleverness.

### 6.1 Primary fix (required) — back off the poll loop
**File:** `app/src/services/websocketService.ts`, the re-chain in `poll()`.
Today: `setTimeout(poll, max(0, POLL_MS - elapsed))` — a new connection every ~1–2 s forever during an outage.
Change: when polls are failing, grow the interval (e.g. 1 s → 2 → 4 → 8 → cap ~15–30 s); reset to 1 s on the first success. ~5 lines, one new counter.
Effect: cuts outage-period connection creation from ~30/min to a handful/min — below the jam threshold — while keeping full 1 Hz telemetry on a healthy link. This is the single highest-leverage change and addresses the always-present churn engine.

### 6.2 Secondary fix (strongly recommended) — don't backfill while the live link is down
**File:** `app/src/services/telemetryLogger.ts`, `pumpBackfills()`.
Today it proceeds whenever `getCurrentIP()` is set, which stays true throughout an outage.
Change: gate on live-link health — `import { isConnected }` and bail out of the pump while `!isConnected()`. ~2 lines.
Effect: removes churn engine #2 during the outage window (when its retries almost always fail anyway — "syncing 0 of N"). **The feature is preserved:** the gap stays queued and backfills as before once the live link is healthy again, which is also when the pull is most likely to succeed.

### 6.3 Optional simplification (removal, if you want less code)
The `/history` backfill carries real machinery: the `pendingGaps` queue, the retry-every-tick pump, and the `syncSynced` / `syncTotalEst` / `syncProgress` sync-progress state + banner. With 6.2 in place, backfill only ever runs on a healthy link, so the "retry forever through a bad link" design is no longer needed. It could be replaced by a **single bulk `/history` pull triggered once the live link is confirmed stable**, deleting the per-tick pump and the progress-tracking state. This removes code and removes churn. Not required to fix the jam — offered as a simplification.

### 6.4 Maximal simplification (a feature decision, not required)
If the unbroken-log-across-dropout feature is judged not worth its cost, removing **store-and-sync entirely** (firmware `histlog` + `/history`, app `historyService` + all backfill/merge/sync-progress code) deletes churn engine #2 outright and removes several hundred lines across both codebases. This is a **product decision for the operator**, listed only because the brief invited removing features. The jam is fixed without it.

### 6.5 Firmware defense-in-depth (lower priority)
The deeper fragility is that one abandoned socket costs 5 s of total server availability, so **any** misbehaving client can jam or crash the boat. Hardening options (shorter per-client request timeout, capping/reaping half-open connections) are worthwhile on a vehicle but are **not the simple path** — `HTTP_MAX_DATA_WAIT` is a library `#define` and the server loop is Arduino-core code. Treat as follow-on, not the fix.

### 6.6 What NOT to do
- **Do not** implement the firmware WiFi retry-cadence / periodic re-scan change (previously held). The boat's association is exonerated; it is cancelled permanently.
- **Do not** shorten the poll timeout to "recover faster" — that _increases_ connection churn, the opposite of the fix.
- **Do not** treat `wifi_assoc` as a liveness signal.

---

## 7. Recommended plan
1. Ship **6.1 (backoff)** and **6.2 (gate backfill)** together — small, contained, feature-preserving.
2. Re-run the range excursion; confirm the post-reconnect lockout is gone and reconnection is prompt.
3. Consider **6.3** as a follow-on cleanup once the fix is validated.
4. Log **6.5** as a firmware robustness item for later.

## 8. Validation plan (before the next mission)
- **Bench, no boat risk:** point the fixed app's poll/backfill logic at a local server that stalls responses; count connections opened per minute during a simulated outage. Expect a large drop vs today.
- **Bench, with boat (spaced, power-cycled between runs):** force an outage (block the link) with the fixed app; confirm the boat does not jam and reconnection is fast once the link returns. Reuse `boat_probe.py` (fixed, no catch-up burst) as the independent witness.
- **Field:** the range excursion that reproduced it — walk out and back; confirm no multi-minute lockout and no `session_id` change.

## 9. Artifacts
- `scratchpad/boat_probe.py` — dual-client independent witness (catch-up bug fixed).
- `scratchpad/churn_test.py` — half-open lockout reproduction (pre-registered predictions).
- `scratchpad/control.py` — clean-close control (isolates the teardown variable).
- `scratchpad/drain_test.py` — reboot-aware v2 (per-poll `session_id`/`heap`).
- Observability already shipped and retained: `js_freeze` (JS-thread heartbeat), `poll_fail` (per-gap failure tally), boat-side `wifi_assoc` + `rssi` in the history ring.
