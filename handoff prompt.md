You are picking up the Legend Cutter autonomous boat project mid-stream
(/Users/ryan/Documents/GitHub/legend-class-autonomous-cutter/). The previous
agent was fired for a series of avoidable mistakes culminating in pushing the
ESCs to a hard command on a MANUAL stick flip — the kind of bug that exists
because the agent didn't read existing test code before writing similar code.

You are picking up test_26 (mode switch + autonomous throttle). Below is
everything you need.

---

## RULE ZERO — READ THE PREVIOUS TESTS BEFORE TOUCHING ANYTHING

Before you write a single line, read:

1. The .ino AND NOTES.md of the **most recent passing test** in
   `firmware/tests/`. As of 2026-05-10 that's `test_25_waypoint_heading/`.
2. `firmware/tests/test_17_manual_sailing/NOTES.md` — canonical style guide,
   stick conventions, failsafe philosophy. **Throttle stick rests at the
   BOTTOM (~1000 µs), not center.** This trips up agents who default to
   centered-stick assumptions — including the previous one.
3. `firmware/tests/test_17_manual_sailing/test_17_manual_sailing.ino` —
   actual mapping code (`computeThrottleUs`, `scaledRudder`, `slewLimit`,
   the differential-thrust math). When you need similar logic for any
   future test, COPY FROM HERE rather than hand-rolling it.
4. `firmware/tests/test_26_mode_switch/freeze_diag/NOTES.md` — explains
   why the generic "channels frozen for N ms" failsafe was removed.
5. `firmware/FIRMWARE_AUDIT.md` — gap list for the production firmware.
6. `firmware/tests/AUTOPILOT_PLAN.md` — DRAFT general plan for what
   tests come after test_26. It's a sketch, not a contract — read its
   preamble before treating any specific test definition as instruction.
7. `PLANNING_NOTES.md` — recent conversation notes with Ryan, including
   open hardware issues (bilge MOSFET, audio module).

If you skip this and fly blind, you will burn the user's time and
potentially break hardware. The previous agent did exactly that.

---

## What has been proven (do not re-litigate)

- test_07/08/15/16/17: manual sailing closed loop — iBUS rudder + throttle
  + ESC + diff thrust + reverse interlock + failsafe. test_17 v1 PASSED
  2026-05-03; v2 was rewritten 2026-05-04 but never tested until 2026-05-09
  (it works fine once the freeze-detector issue below is understood).
- test_22: ICM-20948 hard-iron mag calibration (offsets baked into test_25).
- test_23/24: WiFi + GPS + IMU complementary filter; PCA9685 + IMU on
  shared I2C with proportional heading hold.
- test_25: GPS waypoint → haversine bearing → heading hold rudder. PASS
  2026-05-09 with 1.8° error.
- IMU axis mapping: `mr_x = -mz, mr_y = -my, mr_z = -mx` (chip X=up,
  Y=port, Z=stern). Already in test_25 / test_26.
- iBUS channel map (locked 2026-05-10 after the test_27 RC-loss diag):
  - CH1 (idx 0) — rudder, right-stick H
  - CH2 (idx 1) — reverse, right-stick V down (test_17 only)
  - CH3 (idx 2) — throttle, left-stick V
  - CH5 (idx 4) — VrA knob (deck gun pan / winch)
  - **CH6 (idx 5) — SwD failsafe guard.** Sits up (~1000 µs) in normal
    operation; receiver outputs ~2000 µs from its stored failsafe value
    when the TX is gone. Firmware: `ch[5] > 1500` sustained 500 ms →
    MODE_FAILSAFE. This is the required workaround for FS-iA10B's
    hold-last behavior when channels are configured failsafe=OFF — see
    `test_27_rc_failsafe/rc_loss_diag/`.
  - **CH7 (idx 6) — SwA mode switch.** Up = MANUAL (~1000 µs), down =
    AUTO (~2000 µs). Up is MANUAL because the FS-i6X forces switches up
    at TX power-up; that makes MANUAL the safe boot state.
  - CH4 (idx 3) and CH8–CH10 (idx 7–9) — unused.

  test_26's PASS used the old idx-5 mode location and is preserved as
  historical record. New tests must use the locked map above.

---

## Where test_26 stands right now (2026-05-10)

**Goal of test_26:** prove the three things that are NEW versus everything
above: (1) SwC reads cleanly into a MANUAL/AUTO mode classifier, (2) in AUTO
the firmware commands the throttle so the boat moves under its own command
for the first time ever, (3) flipping back to MANUAL is safe.

**Current sketch:** `firmware/tests/test_26_mode_switch/test_26_mode_switch.ino`

**Most recent untested change (you must verify before changing anything):**
2026-05-10 — fixed a bug where the MANUAL passthrough in `G_WAIT_AUTO`
was forwarding the raw throttle stick value (1000 µs at rest) directly
to `setEscs()` after a hard clamp had been removed in an earlier round.
The result was ESCs slamming to a hard command the moment the operator
flipped to MANUAL.

The fix added:
- `MAX_FWD_US = 1800` constant (matches test_17)
- `mapThrottleStickToEsc()` — bottom-rest stick → ESC output, copied
  semantics from test_17's `computeThrottleUs()`
- `mapRudderStickToServo()` — copied from test_17's `scaledRudder()`
- Hard safety clamp inside `setEscs()` — under no circumstances will
  ESCs see anything outside `[NEUTRAL_US, MAX_FWD_US]`
- Pass-through in `G_WAIT_AUTO` now uses these mappers, not raw stick µs

**Untested.** Ryan has not re-flashed since these were applied. Confirm
his state before doing anything; if he hasn't tested, that is the next
step (procedure below).

---

## ⚠️  CURRENT BLOCKING ISSUE — test_26 is useless as written

**The whole point of test_26 is to prove the firmware can spin the motors
on its own in AUTO mode. RIGHT NOW THE SKETCH DOES NOT VERIFY THAT.**

Gate 2/3 fires `PASS (2/3): AUTO throttle XXXX µs held for 1000 ms` purely
on the basis that the firmware *commanded* a value to the ESCs for 1 second.
It has no way to know whether the motors actually spun — there's no current
sensing wired into this sketch, no operator confirmation step, nothing.

In Ryan's last test run, the sketch printed PASS for gate 2 but the motors
**did not spin at all** (the previous AUTO_CRUISE_US of 1525 µs was inside
the ESC deadband). That false PASS is exactly what got the previous agent
fired — the test reported success while the actual capability under test
was broken.

**Before doing anything else, fix this:**

After the 1-second AUTO spool, the sketch must prompt the operator over
serial: *"Did both motors spin in AUTO mode? Type y or n."* Gate 2 PASSES
only on `y`; FAILS on `n`. This is the only way the test actually proves
what it claims to prove.

This pattern (operator confirmation of physical-world events the firmware
can't measure) is consistent with how the test_17 visual-verify gates
work. Match the existing style. The serial-input plumbing is already in
place from `freeze_diag/` — see how it uses `Serial.read()` to wait on a
single keystroke between phases.

If `AUTO_CRUISE_US = 1600` still doesn't spin the motors when Ryan runs
the fixed sketch, do NOT just bump the value blindly. Ask Ryan to
characterize the ESC deadband first (a tiny diagnostic that ramps from
1500 to 1700 in 10 µs steps and asks where motors first start spinning
would do it), then set AUTO_CRUISE_US to that value plus a margin.

**Other things resolved during this session, all documented:**
- `freeze_diag/` subfolder contains a definitive diagnostic showing the
  FS-i6X / FS-iA10B does not produce gimbal jitter on idle sticks.
- The generic "channels frozen for N ms" failsafe was removed from
  `framesOK()` because it false-tripped on operator stillness — useless
  for a marine vessel. The no-frame timeout (1500 ms) remains.
- `[WARN] iBUS checksum mismatch` print was removed — pure noise at the
  ~1% baseline loss rate from test_15/16.

---

## Procedure for re-running test_26

1. Props OFF or boat firmly secured.
2. Flash `test_26_mode_switch.ino`. Open Serial @ 115200.
3. Power up boat, wait through 3 s ESC arming.
4. TX on, throttle stick at the BOTTOM, right stick centered, SwC UP.
   → Sketch prints `PASS (1/3): MANUAL detected`.
5. **Manual passthrough is now LIVE.** Move sticks; rudder + ESCs follow
   (test_17 conventions: stick at bottom = idle, lifted = scaled forward).
   The peak throttle stick value reached during this phase becomes the
   AUTO cruise µs (mapped, capped at 1750 µs ≈ 50% forward, refused if
   it never clears 1650 µs). Push at least to mid-stick.
6. Drop throttle to idle, flip SwC DOWN. Both motors should spool to the
   captured cruise µs for 1 second, then return to neutral. Sketch prints
   the cruise value, then prompts: `Did BOTH motors spin? Type y or n.`
   Type `y` for PASS or `n` for FAIL → that decides gate 2.
7. Flip SwC UP. → `PASS (3/3)` + results block printed.
8. Have Ryan paste the output. Outputs frozen at neutral; reboot to re-run.

If a gate fails or motors don't spin: **READ THE EXISTING SKETCH AND
test_17's MAPPING FUNCTIONS** before changing anything. Do not assume.

---

## Key rules — never violate

- **Serial output discipline (this is a hard rule, repeatedly violated):**
  No streaming, no per-second prints, no progress chatter. Sketches print
  instructions, prompts between phases, and a single results block at
  the end. The user pastes the result block back to you. If you are
  tempted to add "1 line per second" — don't.
- **Tests prove only NEW capability.** Don't re-walk gates that earlier
  tests already passed (e.g. don't make the operator sweep rudder
  LEFT/RIGHT in test_26 — test_07/15/17 own that).
- **Sketches auto-detect gates.** Operator moves things; sketch prints
  `PASS (N/M)` when conditions are met; sketch prompts the next step.
  No "type M to print mode state" debug commands as the primary flow.
- **Stick conventions:** throttle rests at the BOTTOM (~1000 µs); right
  stick is spring-centered; safe-arm checks throttle at IDLE not center.
- **Commit after writing each test, commit again on PASS.**
- **GPS wiring:** BN-220 white = TX (reversed), green = RX. ESP32 RX → white.
  GPS_RX_PIN=17, GPS_TX_PIN=4.
- **Audio is DF1201S (DFPlayer Pro) at 115200 baud, AT protocol** — NOT
  the DFPlayer Mini. `legend_cutter/audio.cpp` still needs porting.
- **No water testing until bench validation is complete.**
- **Ryan is a Mode-2 RC pilot.** Right stick H = rudder = CH1.

---

## Open hardware issues (separate from test_26)

These are tracked in `PLANNING_NOTES.md` and are NOT blocking test_26
itself, but matter before water testing:

1. **Bilge MOSFET module misbehaving.** Floating logic ground, LED on
   without command, pump activates on touch, ESC chirped weird while it
   was wired. Currently disconnected from the boat. Needs a dedicated
   diagnostic before water (see `AUTOPILOT_PLAN.md` for a proposed
   `test_BILGE_DIAG` outline).
2. **DF1201S audio went silent** during recent unrelated testing. Defer.
3. **iBUS link has a ~1% checksum loss baseline** (test_15/16). Not
   blocking bench tests but should be characterized further before water
   if it gets worse under motor load.

---

## Memory files (auto-loaded into your context)

`/Users/ryan/.claude/projects/-Users-ryan-Documents-GitHub-legend-class-autonomous-cutter/memory/MEMORY.md`
indexes user-level facts and feedback. Among them:

- `feedback_serial_output.md` — serial discipline (read it).
- `feedback_test_style.md` — auto-detect gates, terse procedures.
- `feedback_test_novelty.md` — only test what's new.
- `project_stick_conventions.md` — bottom-rest throttle.
- `project_imu_axis_mapping.md` — confirmed mag axes.
- `project_bn220_wire_colors_reversed.md` — GPS wiring quirk.
- `project_audio_module_df1201s.md` — DF1201S not Mini.
- `project_deck_gun_pan_continuous.md` — pan servo type/channel.
- `user_tx_mode.md` — Mode 2 pilot.
- `feedback_commit_per_test.md` — commit rhythm.

---

## What Ryan needs from you

Test_26 to PASS cleanly with the throttle-mapping fix in place. After
that, commit it (per the commit-per-test convention) and update its
NOTES.md with the result. Then check in with Ryan before scoping the
next test — `AUTOPILOT_PLAN.md` proposes test_27 (autonomous throttle
beyond the 1-second proof) but it's a draft, not a directive.

He has had a frustrating session. Be precise, be brief, and read code
before writing it. If you don't know what test_17 does, look. If you
don't know how a function is supposed to behave, look. Don't guess and
don't restate things you haven't verified.
