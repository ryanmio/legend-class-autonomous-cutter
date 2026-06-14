# Production port plan — test_29 → `legend_cutter/` (the final firmware)

**Status:** planning. Not started.
**Owner:** porting agent (see kickoff brief at the bottom).
**Created:** 2026-06-14, after `test_29-pool2.6-magcal2` PASSED its lake run
(first autonomous waypoint capture).

---

## 1. The decision (context from the planning conversation)

We are doing a **one-time cutover** from the test-sketch track to the
modular production firmware. After this:

- `firmware/legend_cutter/` becomes **the firmware flashed to the boat**
  and the only place development continues, under semantic versioning.
- The `firmware/tests/test_NN_*` sketches become **read-only reference
  oracles** — preserved in git, re-flashable to A/B a suspected
  regression, but never a parallel development line again.

**Why now:** every meaningful water test already re-validates the whole
boat from scratch (new conditions each run). That re-validation is a cost
we pay regardless of which firmware is underneath. So we fold
*port-validation* into the next water bring-up instead of paying it
twice — port once now, validate once, then live on the production stack.
Continuing to tune on a throwaway test sketch and porting "later" just
means porting twice and accreting real work on a disposable sketch.

## 2. Prime directive: ACCURACY AND COMPLETENESS

This is the whole ballgame. Two things have gone wrong before and must
not happen again:

1. **Prior ports were PARTIAL.** `legend_cutter/` is NOT a trustworthy,
   merely-stale copy of the boat's behavior. Some modules are missing
   recent capability, some may have silently diverged even on old
   behavior. **Do not assume any existing `legend_cutter/` module is
   correct.** Every module is verified against the source of truth, not
   delta-patched.
2. **This is a full reconcile, not a delta-apply.** The goal is that
   `legend_cutter/`, flashed to the boat, behaves **identically** to
   test_29 as it stands today — every pin, channel, threshold, FSM
   transition, telemetry field, and HTTP/WS endpoint.

### Source of truth

`firmware/tests/test_29_pool_integration/` at commit **`7b0f7cdd`**
(current `HEAD`, the PASS'd `test_29-pool2.6-magcal2` build) is the
**single source of truth**. Its `.ino`, `config.h`, `secrets.h`, and
`NOTES.md` define correct behavior. Where `legend_cutter/` disagrees with
test_29, **test_29 wins** — no exceptions, no "improvements."

## 3. Goals — what "done" means

- `legend_cutter/` is a complete, faithful, modular representation of the
  PASS'd test_29 firmware. Nothing the boat does today is missing or
  changed.
- `FIRMWARE_VERSION` = **`0.4.0`** (drop the `-poolN` suffix; clean semver
  from here — MAJOR stays 0 until the project is called "done").
- The **phone app needs zero changes**: it is already updated for the
  magcal2 feature set and is firmware-agnostic *as long as the telemetry
  field names and endpoint paths match test_29 exactly*.
- `CLAUDE.md` and the relevant memory files are updated so the *next*
  session knows `legend_cutter/` is live and the test→port workflow is
  retired.
- Validated by the operator on the boat (bench parity, then water), not
  by the agent claiming success.

## 4. Guardrails (read before touching code)

- **Faithful port first, improvements never (here).** Do NOT "fix,"
  refactor-for-taste, retune, or improve behavior during the port. A
  byte-for-byte-equivalent boat is the success criterion. The open items
  (PID smoothing, TX-off failsafe, `mag_cal_circ_pct` NVS gap, `batt_a`
  reading 0) are explicitly **out of scope** — they are the *first work
  on the new stack after cutover*, as separate versioned changes.
- **The app contract is law.** Telemetry JSON keys and HTTP/WS routes
  must match test_29 exactly. Any drift silently breaks the only
  water-side operator surface. If reconciling `telemetry.cpp` is messier
  than rewriting it, a clean rewrite that reproduces the exact same wire
  contract is acceptable (operator has no preference on internals).
- **`config.h` is the single source of hardware truth.** Reconcile it
  first; everything downstream depends on it. The "hardware facts that
  bite" in `CLAUDE.md` are mandatory checks: Mode-2 steering on CH1;
  throttle rests at bottom (~1000 µs); DF1201S audio (AT @115200,
  pause+50 ms before play); BN-220 reversed wires (white=TX,
  `GPS_RX_PIN=17/GPS_TX_PIN=4`); IMU remap `mr_x=-mz,mr_y=-my,mr_z=-mx`;
  deck-gun pan servo on **PCA ch8** (positional, knob=CH5);
  `MAG_DECLINATION_DEG = -11.0`.
- **Cannot flash, cannot self-validate.** The agent has no hardware. All
  PASS/FAIL verdicts come from the operator. Never mark a gate PASS or
  claim a behavior works without operator confirmation.
- **Preserve the test sketch.** Do not edit or delete
  `test_29_pool_integration/`. It is the oracle.
- **Serial discipline & code-style guardrails carry over** (≤1 Hz
  summary lines, no history comments in source, one-clause `#define`
  comments, no `/sim_gps` UI affordance, WiFi = home/hotspot only).
- **Reviewable, bisectable commits.** Lean toward subsystem-grouped
  commits (not one giant squash, not per-file noise) so a bench-parity
  regression can be bisected — but the exact slicing is yours.

## 5. The plan — outcome gates, your decomposition

**This is a skeleton and a set of gates, not a script.** After the Stage 0
audit, *you own how the reconcile is sliced and ordered* — by module, by
risk, by dependency, however the gap map tells you. The firm parts are the
guardrails (§4) and each gate's exit criterion; the *how* — and any
re-planning the audit justifies — is yours. Keep your own finer-grained
task list as you work.

You **can compile-verify yourself** (arduino-cli) — only *flashing* the
boat needs the operator. So "clean build" is your gate to clear, not
something to defer.

- **Stage 0 — Setup & parity audit (always first).** Copy `secrets.h`
  into `legend_cutter/` (it's gitignored — never commit it). Extract the
  behavioral spec from the source of truth (every config constant,
  telemetry field, endpoint, FSM transition, module behavior in test_29)
  and audit current `legend_cutter/` against it. **Gate:** a written
  per-module gap/divergence map — missing / stale / wrong / **extra**
  (flag anything `legend_cutter/` does that test_29 does *not*; don't
  silently keep or drop it). This sizes and orders everything after it:
  do the audit before writing port code, then **continue straight into
  Stage 1** — run the port through without pausing for sign-off. (The
  only natural stops are the operator gates at Stages 2 and 4, which need
  hardware you don't have.)

- **Stage 1 — Reconcile to parity (you decompose this).** Bring every
  `legend_cutter/` module into faithful parity with test_29, decomposed
  and sequenced as the audit dictates. Firm gates regardless of slicing:
  - *Config first* — `config.h` is the hardware-truth dependency
    everything reads; reconcile it before the modules that depend on it,
    with no accidental drift from test_29.
  - *App contract* — emitted telemetry JSON keys and HTTP/WS routes match
    test_29 exactly (rewrite `telemetry.cpp` if cleaner than reconciling —
    only the wire format is sacred); the app needs zero changes.
  - *Behavioral parity* — every behavior the boat has today is present and
    unchanged: sensors, mag-cal + NVS + true-heading, motors/diff-thrust,
    navigation (heading-hold, COG-trim, capture, waypoint guard), FSM
    (MANUAL/AUTO/FAILSAFE + re-arm), comms, bilge, lights, audio, weapons,
    radar, sonar.
  - *Clean build + version* — full sketch compiles; `FIRMWARE_VERSION` →
    `0.4.0`.
  Keep a verification trail mapping test_29 facts to their new home so the
  parity claim is checkable, not asserted.

- **Stage 2 — Bench-parity validation (operator).** Operator flashes
  `legend_cutter/` and re-runs the accumulated bench gates (AR-*, MC2-*,
  TB-*) on stands; confirms identical behavior to test_29 last-known-good
  and that the app parses all telemetry + endpoints. **Gate:** operator
  confirms parity.

- **Stage 3 — Cutover housekeeping.** Update `CLAUDE.md` (legend_cutter is
  the live flashed stack; test_NN are archived references; rewrite the
  "Test → production workflow" + "active flashed sketch" sections) and the
  memory files (`project_active_firmware_sketch`,
  `feedback_test_first_then_port`, MEMORY.md index). **Gate:** docs/memory
  describe the new reality.

- **Stage 4 — Water validation (operator).** Lake run on v0.4.0 doubles as
  port-validation and the first post-cutover feature test — the start of
  normal development on the new stack. Listed for sequencing; not part of
  "the port" itself.

## 6. Milestone checklist (maintain your own finer list beneath these)

- [ ] Stage 0 — `secrets.h` copied (uncommitted); per-module gap audit written
- [ ] Stage 1 — config reconciled (no drift)
- [ ] Stage 1 — app-contract parity (telemetry keys + routes; app unchanged)
- [ ] Stage 1 — behavioral parity across all modules
- [ ] Stage 1 — clean full-sketch compile; `FIRMWARE_VERSION` → `0.4.0`
- [ ] Stage 2 — operator bench-parity PASS
- [ ] Stage 3 — `CLAUDE.md` + memory updated for the cutover
- [ ] Stage 4 — operator water validation (begins v0.4.x development)

## 7. Decisions already made

- Version: **0.4.0**, semver, drop `-poolN`. ✔ (operator)
- Cutover is **one-time and final**; test track retired. ✔ (operator)
- Telemetry rewrite-vs-reconcile: agent's call, as long as the wire
  contract is identical. ✔ (operator: "don't care")

---

## Appendix — Agent kickoff brief

> You are porting this project's firmware to its final form. The boat
> currently runs `firmware/tests/test_29_pool_integration/` (commit
> `7b0f7cdd`), which just PASSED a lake test — it autonomously navigated
> to a waypoint for the first time. Your job is a **one-time, complete,
> faithful port** of that firmware into the modular
> `firmware/legend_cutter/` stack, which then becomes the live firmware
> for the rest of the project under semantic versioning. The test sketch
> is retired to a read-only reference after this.
>
> **The hard part is accuracy, not feature work.** Prior ports into
> `legend_cutter/` were partial, so you cannot trust its existing modules
> to be correct — treat test_29 (commit `7b0f7cdd`) as the single source
> of truth and verify *every* module against it. The flashed result must
> behave identically to test_29 today: same pins, channels, thresholds,
> FSM transitions, telemetry fields, and endpoints. Do **not** improve,
> retune, or refactor-for-taste — a faithful port is the only goal; known
> open items (PID smoothing, TX-off failsafe, etc.) are deliberately for
> *after* the cutover.
>
> Read `firmware/PRODUCTION_PORT_PLAN.md` for the full context,
> guardrails, and the staged plan, then read `CLAUDE.md`, the test_29
> `.ino` + `NOTES.md`, and every existing `legend_cutter/` module before
> writing anything. **Start by producing the Stage 0 parity/gap audit —
> do not write port code until the gap map exists.** Critical constraints:
> the phone app is already done and must need zero changes, so telemetry
> JSON keys and HTTP/WS routes must match test_29 exactly; `config.h` is
> the single source of hardware truth and must be reconciled first; you
> cannot flash hardware, so the operator owns all PASS/FAIL verdicts —
> never claim a behavior works without their confirmation. Work in staged,
> subsystem-grouped commits. Surface any genuine ambiguity instead of
> guessing.
