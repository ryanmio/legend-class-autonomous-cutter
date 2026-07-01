I want to scope the planning of multi-waypoint mission planning. The goal would be to extend the current single waypoint to string together multiple waypoints and then capture them all. I don't know for sure that i want to implement this. I want you to scope it out and tell me what it would take, and the approach you recommend. The goal would be to complete a 1 mile run in the potomac autonomously. I'd enter multiwaypoint mode on my phone app, place each waypoint, and confirm route before it is sent to the boat. Then when i enable auto mode on the boat's radio controller, it would execute the waypoints in order. All the existing safeguards would still be the same. switching to manual would immediately let me takeoover, and flipping failsafe would stop everything and then i can re-engage manual. I am worried about adding too much complexity to the project, but I think this would be a big value add. The main benefit I see is that currently the auto mode is limited by Wifi range. I connect to the boat with the app using my phones' hotspot, or rather the boat connects to my phone's hotspot. then i can post the waypoint coordiante and engage auto with RC. But once/if the boat leaves wifi range, i can no longer add more waypoints. I want to do a big 1 mile autonomous route in the potomac as my big proof of concept that i built a successful ship myself, and i plan to walk alongside the bank of the river on a trail where i can stay within range so that i can keep adding waypoints. However, if at any pooiint i lose wifi, the "full auto for 1 mile" thing will be broken. So i'd rather pre-set the route, then walk alongside. If i need to change the waypoints mid-flight, i could do so if i have wifi and if i dont i can go into manual and steer it closer until i get wifi and then update the waypoints then continue. I'm not really sure if this is a good idea though because if i do single waypoints ill have better sense of where the boat actually is relative to the waypoints im placing like i can see where ive been placing waypoints on the map and how close the boat is to shore and adjust in real time either out more or in more. i could still edit the waypoint mission if i have wifi if we buuld edit into the multipoint but that's a whole nother layer i feel like. do you have a good idea of what im trying to do? And my preference for keeping things simple in the code and project in general but also my feeling that this may be a time it is worth the added complexity? Ask any questions, then consider different approaches, then recommend the best (simplest and most reliable) approach here in this document.

---

# Multi-Waypoint Mission Planning — Scope & Recommendation

## Do I understand what you're after?

Yes. The core problem isn't "I want fancier autonomy" — it's **WiFi range**. Today AUTO
can only ever know about one waypoint, and you can only post that waypoint while the boat is
in hotspot range. For a 1-mile Potomac run you'd be feeding waypoints one at a time as you
walk the bank, and the instant you lose the link mid-river the "full auto for a mile" claim
breaks. The fix is to get the **whole route onto the boat before launch**, so the mission
completes even if the link drops to zero.

Everything else you described is a consequence of that: pre-plan + confirm on shore, engage
AUTO on the RC, boat runs the legs in order. All existing safeguards identical — MANUAL
takes over instantly, FAILSAFE stops everything and you re-engage from MANUAL.

Your two scope calls (locked):
- **Editing**: only if it's simple and reliable. It is — see "Editing comes for free" below.
- **UX**: a **separate PLAN mode**. Today's instant tap-to-place single-waypoint flow stays
  byte-for-byte unchanged and remains the default. You keep the real-time feedback workflow
  you like; multi-waypoint is a deliberate, separate act.

## The one design principle that keeps this simple

**The boat holds a dumb ordered array and always drives from index 0. It never edits, never
reconciles, never reasons about "what changed."** It captures the active leg, advances the
index, and neutral-holds after the last point — exactly the capture behavior it has today,
just looped. *All* the smarts (planning, editing, "resume from here") live in the app, where
the map and your finger already are.

This is what makes editing free and the firmware boring (boring = reliable on the water).

## Approaches considered

**A — Firmware holds the route, auto-advances. ← RECOMMENDED**
App sends the full ordered route once; firmware stores it, drives leg 0→1→...→N, neutral-holds
at the end. Survives total WiFi loss, which is the entire point.

**B — App sequences single waypoints (post the next WP each time `captured` flips).**
Rejected. It requires a *live link for the whole mission* — the exact failure mode you're
trying to eliminate. Lose WiFi at leg 3 and the boat sits at leg 3's captured point forever.

**C — Full live edit (insert/move/delete individual points mid-mission, firmware reconciles
against progress).** Rejected *for now*. Most state to get wrong on both sides, and the
"resend-from-here" trick below gets ~90% of the value at ~10% of the risk. Revisit only if
real use shows you need surgical mid-flight edits.

## Prior art — `test_28_multi_waypoint` already proved most of the firmware

This is not a from-scratch build. `firmware/tests/test_28_multi_waypoint/` is a **PASS'd
(2026-05-10) reference oracle** that already implements the exact sequencer this needs, and was
kept out of the pool sketch *only because the pool was too small* — the constraint you just lifted.
It proved, on the bench via `/sim_gps`:

- Volatile mission array (up to 32 waypoints), `POST /mission` (top-level JSON array) + `/mission/clear`.
- Sequencer: advance `wp_idx` on capture; at `wp_idx >= wp_count`, `mission_active=false` +
  single `MISSION COMPLETE` log; if AUTO at completion, ESCs neutralized.
- Telemetry fields `mission_active`, `wp_count`, `wp_idx` (plus the existing `wp_dist_m`/`wp_bearing`).
- FAILSAFE **pauses, doesn't reset** — `wp_idx`/`mission_active` survive the round-trip.

So the firmware task is a **port + adapt**, and the two adaptations are the only real new firmware work:

1. **Marry test_28's sequencer to `legend_cutter`'s *better* per-leg capture.** test_28 used the
   old 3 m distance-only capture. `legend_cutter` has the superior model you've since proven:
   `CAPTURE_RADIUS_M=5` **OR** perpendicular-crossing **+** approach-heading lock. Keep
   `legend_cutter`'s capture/leg-start logic; just have a capture *advance the index* instead of
   ending the run. Also keep `legend_cutter`'s **AUTO-gated** capture (test_28 ran the sequencer
   in any mode to prove it with motors silent — that was a bench affordance, not what you want on
   the water).
2. **Cross-core handoff.** test_28 predates the v0.6.0 networking-off-the-control-loop split, so
   its HTTP handler wrote the array directly. In `legend_cutter` the array is read by the control
   core, so the array must arrive via the staging-buffer + single-commit-command pattern (below).

Adopt test_28's proven **names and shapes** rather than inventing new ones: `POST /mission` body is
a **top-level JSON array** `[{"lat":..,"lon":..}, …]`; telemetry is `mission_active` / `wp_count` /
`wp_idx`; clear via `POST /mission/clear`. (Read its `NOTES.md` before coding — it documents the
parser error cases and the deliberate simplifications.)

## Recommended approach (A), in detail

### Firmware (port test_28 sequencer into `navigation.cpp` + adapt, MINOR bump → v0.7.0)

The whole change is: replace the single `wpLat/wpLon` with a small array + an active index.
Every existing getter (`navWpLat()`, `navWpBearing()`, `navCaptured()`, …) returns the
**active leg**, so the `MODE_AUTO` branch in `legend_cutter.ino`, all `wp_*` telemetry, and
the `MapScreen` HUD keep working with **zero changes** — they just follow whichever leg is live.

- `static Waypoint waypoints[MAX_WAYPOINTS]; static uint8_t wpCount, wpIdx;`
  Keep test_28's `MAX_WAYPOINTS = 32` (32 × 8 B = 256 B RAM — nothing; ≈ a mile at ~50 m spacing).
- **Auto-advance**: in `navUpdate()`, when the active leg is captured (distance OR crossing —
  `legend_cutter`'s logic, unchanged): if `wpIdx < wpCount-1`, do exactly what `navTrySetWaypoint`
  already does on a new point (`captured=false`, `startValid=false`, `approachLocked=false`) and
  `wpIdx++`. If it was the last leg, leave `captured=true` / `mission_active=false` → neutral hold
  = **identical to today's post-capture behavior**. The advance reuses the existing per-leg reset,
  so the proven crossing/approach-lock geometry just chains.
- **`POST /mission`** — top-level JSON array `[{"lat":..,"lon":..}, …]` (test_28's proven shape):
  - Validate **per-leg (chained)** distance — this is the one real gotcha. Today's
    `MAX_WP_DIST_M = 1000` fat-finger guard is measured boat→waypoint; a pre-planned mile route
    has later points >1000 m from the launch spot and would be **wrongly rejected**. For a
    mission, validate each leg as distance from the *previous point* (leg 0 from the boat if a
    fix exists), each ≤ `MAX_WP_DIST_M`. This also naturally enforces sane waypoint spacing.
  - Validate count (1…`MAX_WAYPOINTS`) — test_28's parser already rejects empty / >32 / bad-shape.
- **Command-queue handoff** (the only non-obvious bit, and the second adaptation vs test_28): the
  flat `Command` struct in the 8-deep SPSC ring can't carry 32 points. Use the pattern the codebase
  *already uses* for PID/RADAR ("handler preps, command is a lightweight trigger"): the HTTP handler
  parses + validates the array into a single network-side **staging buffer**, then enqueues one
  `CMD_MISSION_COMMIT`. The control loop dequeues it and copies staging → `waypoints`, sets
  `wpIdx=0`, resets leg state. Missions are human-paced (you don't send two within a control tick),
  so a single staging buffer + a `stagingBusy` flag is safe — same cross-core discipline as the rest
  of the loop, no new lock.
- **`POST /waypoint` stays exactly as-is** — internally just a 1-point mission. The single-
  waypoint flow you like does not change.
- **Telemetry**: add test_28's `mission_active`, `wp_count`, `wp_idx` (0-based). Keep all existing
  `wp_*` fields reporting the active leg.
- **`GET /mission`** (optional, cheap): returns the loaded route so the app can rehydrate the
  planned polyline after an app restart or reconnect.

**Safety — preserved by construction, not by new guards:**
- The mode FSM, MANUAL, and FAILSAFE are **untouched**. A loaded route is just *data that the
  AUTO branch consumes* — there is no new "mission mode" on the boat. MANUAL still drives the
  outputs directly; FAILSAFE still neutralizes; re-engage is still flip-SwA-from-MANUAL.
- `navResetLegStart()` already fires on **every →AUTO transition** (`legend_cutter.ino:117`).
  So if you drop to MANUAL, steer the boat somewhere, and re-engage AUTO, it resumes the active
  leg with a fresh crossing line perpendicular to the path it'll actually drive — **for free**,
  exactly what your "steer it closer then continue" workflow needs.
- Power-cycle wipes the RAM route → boat boots MANUAL with no mission — same safe default as
  today's RAM-only single waypoint.

### Editing comes for free ("resend-from-here")

Because the boat always drives index 0 of whatever array it's given, editing is purely an app
act: re-enter PLAN, the app **pre-loads the remaining (uncaptured) waypoints** using `wp_idx`,
you tweak them, hit confirm, and it re-`POST /mission`s. The boat just replaces its route and
starts at 0. No firmware edit logic, no progress reconciliation, no new failure modes. If you
*don't* have WiFi, you fall back to exactly your stated plan: MANUAL, steer closer, regain link,
resend. This gives you the editing capability you wanted at essentially zero added firmware risk.

### App (~1 day; the PLAN-mode UI is the bulk)

Built entirely inside the existing `MapScreen` Leaflet-in-WebView (RN → WebView via
`injectJavaScript`, WebView → RN via `postMessage`). No new screen.

**Two interaction modes, one toggle.** A `PLAN` pill next to `CRUISE` flips
`screenMode: 'live' | 'plan'`. The toggle only changes what a map tap *means* + whether the
draft toolbar shows.

- **LIVE (default = today's behavior, untouched):** tap water → immediately `POST /waypoint`
  (single point). The instant "just go here" workflow you like is unchanged.
- **PLAN:** tap water → *append* to a local draft list. Nothing reaches the boat until CONFIRM.

**Rendering a mission while the boat moves (LIVE).** Three layers, styled off telemetry
`wp_idx`/`wp_count`:
  - boat marker + steel-blue trail — unchanged (`updateBoat`);
  - the route — numbered markers `1..N` + connecting polyline: `< wp_idx` = done (dimmed/✓),
    `== wp_idx` = active (the bright cyan reticle you already have), `> wp_idx` = upcoming (hollow);
  - active leg — the existing dashed `pathLine`, re-anchored boat→`waypoints[wp_idx]`, redrawn
    each frame.
  - HUD gains a leg prefix: `LEG 2/6 → 041° · 180 m`, and `✓ MISSION COMPLETE` on final capture.
  - **Data source:** `/telemetry` only carries the *active* WP, so the app draws the full route
    from its own `missionRoute` state (it sent it). The one gap — an **app relaunch mid-mission**
    — is why `GET /mission` graduates from optional to **required**: fetch the array once on mount
    to rehydrate the polyline (extends the existing `hydratedRef` one-shot hydrate pattern).

**PLAN mode — add / move / delete (no reorder).** Move + delete both use a **tap-to-select
model, no dragging** — the most reliable pattern in this WebView (drag has tap-vs-micro-drag
ambiguity and small touch targets in sun; long-press `contextmenu` is unreliable on iOS WebView).
Draft points render in a distinct **amber** (draft ≠ live cyan, so "on the boat" vs "still
drawing" is never ambiguous).
  - **Nothing selected** (default): tap water → **add** a point at the end.
  - **Tap a draft marker** → **select** it (enlarges/brightens). A slim action bar appears:
    `#3 selected · [DELETE] [DONE]` with a bold banner *"MOVING #3 — tap map to reposition."*
  - **While selected:** tap water → **move** #3 there (stays selected, so you can nudge again);
    **DELETE** removes it and renumbers; **DONE** (or re-tap the marker) deselects → back to add.
  - This is fully tap-based, every state visually loud, zero drag/long-press. The one modal bit
    (tap-water = move while selected, = add otherwise) is covered by the banner.
  - Bottom toolbar always shows `PLAN · N pts · ~X m` (haversine over the draft chain — the
    `bearingTo`/`distanceTo` helpers already exist) + `[UNDO] [CLEAR] [CANCEL] [CONFIRM ▸]`.
  - **CONFIRM** → `POST /mission`. Success: draft becomes the live mission (recolor cyan), exit
    to LIVE. Reject (per-leg distance guard): firmware returns the offending leg index + distance;
    the app highlights that marker red and stays in PLAN so you fix it and retry.

**Editing a live mission = "resend-from-here."** Entering PLAN while a mission runs pre-loads the
draft with the **remaining (uncaptured) waypoints** (`index >= wp_idx`). Tweak, CONFIRM, resend.
No special firmware path — the boat just replaces its array and drives from index 0.

**WebView bridge additions** (match existing `setWaypointMarker` style): `setMission(points,
activeIdx)`, `setDraft(points, selectedIdx)`, `clearDraft()`, `setTapMode('live'|'plan')`; new
inbound messages `{type:'append',lat,lon}`, `{type:'select',index}`, `{type:'move',lat,lon}`.
Single-waypoint is just a length-1 mission through `setMission` — no separate code path.

- `esp32Service`: add `setMission(ip, points[])`, `getMission(ip)`, `clearMission(ip)`.
- CSV logger: `wp_idx`/`wp_count` ride existing telemetry → add the two columns so the flight
  log shows which leg was active.

## Effort & sequencing

1. **Firmware** (one reviewable commit, v0.7.0): port test_28's mission array + sequencer +
   `/mission` handler + telemetry into `navigation.cpp`, then the two adaptations — marry it to
   `legend_cutter`'s crossing/approach-lock capture (AUTO-gated), and route the array through the
   staging-buffer + `CMD_MISSION_COMMIT` handoff. Plus the per-leg distance validation test_28
   lacked. Lighter than a from-scratch build because the parser/state-machine/telemetry are proven.
2. **Bench-the-boat validation** before water: drive `/mission` + auto-advance with `/sim_gps`
   over curl (no UI affordance — bench only). Confirm legs advance, last leg holds neutral,
   re-engage resumes the right leg.
3. **App** PLAN mode + `setMission`. ~1 day.
4. **Water**: short 3–4 point run in open water with you on MANUAL standby, *then* the mile.

## Risks / watch-items

- **Corner-cutting**: the crossing trigger advances a leg the moment the boat passes the
  perpendicular line, so a tightly-chained route could clip a corner rather than driving to each
  point. Mitigation: you confirm the route on the satellite map first, and capture-radius/
  approach-lock are unchanged from the single-leg behavior you've already proven.
- **The per-leg validation fix is mandatory** — without it a pre-planned mile route gets rejected
  by the existing 1000 m guard. Called out above so it isn't missed.
- **No sticky boat-side mode**: keep "mission" as data, not a boat state. Clearing is
  `POST /waypoint {null}` or an empty `/mission`. This keeps the dangerous-flag-in-water concern
  off the table.

## Bottom line

Worth doing, and it lands on the simple side of the ledger because the firmware stays a dumb
array-walker and all the intelligence stays in the app. The single-waypoint workflow you value
is untouched; multi-waypoint is additive and behind a deliberate PLAN mode; editing falls out
for free; and every existing safeguard is preserved by construction rather than by new code.
Recommend Approach A; PLAN-mode editing = tap-to-select move/delete (no drag, no reorder) +
resend-from-here; defer full mid-flight live-edit until real Potomac use says you need it.