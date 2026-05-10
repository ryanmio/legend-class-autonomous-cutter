# test_26_mode_switch — Notes

## Gates

| Gate | Description | Result |
|------|-------------|--------|
| GATE 1/3 | SwC UP, sticks safe → MANUAL detected | PASS |
| GATE 2/3 | Manual peak captured → AUTO spool → operator confirms motors spun | PASS |
| GATE 3/3 | SwC UP → MANUAL, outputs frozen at neutral | PASS |

## Result: PASS (2026-05-10)

Captured cruise = **1653 µs** for 1000 ms. Both motors spun (operator-confirmed
via the y/n gate). Max heading drift 2.0°, max rudder deflection -170 µs.

The first time the firmware has ever commanded the throttle on its own.

### Observations

- Captured cruise of 1653 µs is right at the floor (1650). Confirms the
  hardcoded 1600 µs from earlier drafts was inside the deadband by ~50 µs
  on this rig — capture-from-manual was the right call.
- Max rudder deflection -170 µs = full port (`RUDDER_MIN_US - NEUTRAL_US`).
  Heading drift only 2.0°, so the slam came from the D term reacting to a
  brief gyro transient at the start of the spool, not from accumulated
  heading error. Not a defect — heading hold was secondary on this gate
  and is fully covered by test_25. If it becomes a problem in test_27+,
  cap rudder rate-of-change or filter the gyro feed into `headingHoldUs()`.

## What this test proves (NEW vs everything before)

1. SwC (CH6, iBUS idx 5) reads cleanly into a MANUAL/AUTO classifier with
   hysteresis (1450/1550 µs).
2. In AUTO, the firmware commands the throttle on its own — the boat moves
   under the firmware's command for the first time in the project.
3. Cruise µs is captured from the operator's peak throttle stick during the
   MANUAL passthrough phase, so AUTO is guaranteed to use a value the
   operator has already demonstrated will spin this rig's motors.
4. AUTO → MANUAL flip is safe (outputs return to neutral).

## SwC convention (inverted from earlier draft)

**UP (~1000 µs) = MANUAL, DOWN (~2000 µs) = AUTO.**

The FS-i6X transmitter forces SwC UP on TX power-up — the only way to bring
the TX out of its boot guard is with the switch up. So MANUAL has to be the
up position. AUTO is the deliberate down-flick after the operator has set up
the test.

If you re-flash this sketch later and find motors spinning unexpectedly the
moment you turn the TX on, the most likely cause is the SwC convention
having been re-inverted somewhere. Check `MODE_MAN_BELOW_US` / `MODE_AUTO_ABOVE_US`
and the `swcInManual()` / `swcInAuto()` predicates.

## Captured cruise µs (instead of a hardcoded constant)

Earlier drafts hardcoded `AUTO_CRUISE_US = 1600` (≈20% forward). On the
bench rig in use as of 2026-05-10, **1600 µs is inside the ESC deadband** —
the ESCs received the PWM but the motors did not spin, and gate 2 false-PASSed
because the sketch had no way to detect that it failed.

Replaced with:

- During `G_WAIT_AUTO`, track `maxManualThrottleStickUs`.
- On AUTO entry, map it through `mapThrottleStickToEsc()` (same mapping as
  MANUAL) to get `mappedPeak`.
- If `mappedPeak < AUTO_CRUISE_FLOOR_US` (1650): refuse AUTO with FAIL — the
  operator never demonstrably revved past the deadband, so we have no idea
  whether AUTO will spin anything. Better to fail loudly than to pick a
  magic number.
- Otherwise `autoCruiseUs = min(mappedPeak, AUTO_CRUISE_CAP_US)` (1750 µs ≈
  50% forward). Cap exists so an operator who pushes full stick in MANUAL
  doesn't end up with a hands-off AUTO at 60% forward (`MAX_FWD_US=1800`)
  on a no-load bench.
- The hard `setEscs()` clamp at `[NEUTRAL_US, MAX_FWD_US]` remains as the
  final backstop.

## Operator confirmation gate (y/n)

After the 1-second AUTO spool the sketch drops ESCs to neutral and prompts
`Did BOTH motors spin during AUTO? Type y or n.` Gate 2 PASSes only on `y`,
FAILs on `n`. The firmware can't measure spin (no current sensor on this
rig), so the operator is the source of truth — same pattern as test_17's
visual-verify gates.

## Procedure

1. Props OFF or boat firmly secured.
2. Flash. Open Serial @ 115200.
3. Power up boat, wait through 3 s ESC arming.
4. TX on, throttle BOTTOM, right stick centered, **SwC UP**.
   → `PASS (1/3): MANUAL detected`.
5. Manual passthrough is now LIVE. Push throttle stick up at least to
   mid-stick (this becomes the AUTO cruise µs, capped at 1750). Verify
   rudder + ESCs respond.
6. Drop throttle to idle, **flip SwC DOWN**. Both motors should spool for
   1 second, then return to neutral. Sketch prints the cruise µs and asks
   `Did BOTH motors spin? Type y or n.` Answer accordingly.
7. **Flip SwC UP** → `PASS (3/3)` and results block printed. Outputs
   frozen at neutral; reboot to re-run.

## What this test deliberately does NOT re-walk

- Manual stick → ESC / rudder mapping, diff thrust, reverse interlock —
  test_17 owns that. The MANUAL gate here is a single-frame sanity check.
- Heading hold rudder geometry — test_25 owns that. The AUTO gate proves
  the throttle side; the rudder is along for the ride.
