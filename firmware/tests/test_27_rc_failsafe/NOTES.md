# test_27_rc_failsafe — Notes

## Gates

| Gate | Description | Result |
|------|-------------|--------|
| GATE 1/4 | Boot, SwC UP, sticks safe → MODE_MANUAL detected | TBD |
| GATE 2/4 | `POST /cruise` + flip SwC DOWN → MODE_AUTO, motors spin (operator y) | TBD |
| GATE 3/4 | TX off → ≤4 s later, MODE_FAILSAFE, motors neutral (operator y) | TBD |
| GATE 4/4 | TX on with SwC=AUTO → ACK_REQUIRED → flip UP to ack → flip DOWN → AUTO re-engages, motors spin (operator y) | TBD |

## Result: PENDING

## What this test proves (NEW vs everything before)

1. **Canonical `Mode` enum** (`MODE_MANUAL`, `MODE_AUTO`, `MODE_FAILSAFE`) wired
   end-to-end through the state machine and applied to outputs by mode, not
   by gate. Same enum the production firmware will inherit.
2. **3 s no-frame RC failsafe** — outputs forced neutral, mode → FAILSAFE.
   Replaces the test_26 1.5 s "halt the test" pattern with a real mode that
   the test continues to operate in.
3. **Sticky ACK** — once FAILSAFE trips, firmware refuses to re-engage AUTO
   even after RC frames return. Operator must flip SwC to MANUAL to
   acknowledge before AUTO can re-arm. Prevents the runaway scenario where
   TX glitches mid-mission and recovers with the switch still in AUTO.
4. **HTTP `POST /cruise`** — cruise µs is set via HTTP (default 0; AUTO
   refuses until set). Same pattern the app will use for water testing.
   Replaces test_26's capture-from-stick mechanism.
5. **HTTP `GET /telemetry`** — exposes mode, cruise, RC age, INA219 voltage,
   fused heading. Single biggest force-multiplier for the first water test
   (test_32 success criterion #6: "no battery surprises") — operator can
   monitor voltage without unscrewing the hatch.
6. **INA219 voltage observability** — polled at 4 Hz, surfaced in
   `/telemetry` only. No battery failsafe behavior in this test (deferred
   to sea trials per conversation 2026-05-10).

## Failsafe semantics (locked in here, inherited by production)

**Trigger:** `millis() - lastFrameMs >= 3000` (3 s no-frame).

**Behavior:** mode → FAILSAFE, `failsafeAckRequired = true`, outputs neutral.

**Recovery (sticky ack):** even after frames return, mode stays FAILSAFE
until firmware observes a clean `swcInManual()` (CH6 < 1450 µs). On that
observation, `failsafeAckRequired` clears, mode → MANUAL. Operator can then
flip SwC DOWN to re-arm AUTO normally.

**Override direction is one-way:** firmware can only *downgrade* the
operator's mode request (AUTO → MANUAL/FAILSAFE), never *upgrade*. SwC=MANUAL
is always honored; SwC=AUTO is conditional on (a) frames good, (b) no ack
pending, (c) cruise µs valid.

**Pre-RC state:** while `ibusEverGood == false` (boot before any iBUS frame),
outputs are forced neutral regardless of mode value. This is *not* the
FAILSAFE state — it's a separate startup safe-hold.

## What this test deliberately does NOT cover

- **GPS-loss failsafe** — defer to sea trials. Without an active GPS-driven
  mission running there's nothing meaningful for GPS loss to interrupt.
- **Battery failsafe behavior** — INA219 voltage is *measured* but no action
  is taken on it. Threshold and "limp home" behavior to be experimented
  with in sea trials per PLANNING_NOTES.md.
- **PD heading hold tuning** — test_26 noted the rudder slamming to full
  port during the AUTO spool. AUTO here uses the same heading hold; rudder
  behavior is along for the ride. Defer to a future PD-tuning test.

## Procedure

The sketch defaults cruise to 1700 µs so the test runs end-to-end without
any HTTP traffic. `POST /cruise` is exercised separately by hitting it from
curl/the app at any time during the run; the value takes effect on the next
AUTO entry.

1. Props OFF or boat firmly secured.
2. Copy `secrets.h.example` → `secrets.h` and fill in WiFi credentials.
3. Flash. Open Serial @ 115200.
4. Power up boat, wait through 3 s ESC arming.
5. TX on, sticks safe, **SwC UP** → `PASS (1/4): MANUAL detected`.
6. **Flip SwC DOWN.** Mode → AUTO at 1700 µs, motors spin. Sketch prints
   `AUTO engaged. Verify motors spinning, then turn off the TX.`
7. **Turn off the TX.** Within 3 s: failsafe trips, ESCs neutral, sketch
   prints `[FAILSAFE] RC lost...` then prompts `Did motors spin in AUTO
   and then STOP after TX off? y/n`. `y` → `PASS (2/4) + (3/4)`, `n` → FAIL.
8. **Turn TX back on with SwC still in the AUTO position.** Sketch prints
   `[FAILSAFE] frames restored but ACK_REQUIRED`. Mode stays FAILSAFE.
9. **Flip SwC UP** to acknowledge. `[FAILSAFE] cleared. mode=MANUAL`.
10. **Flip SwC DOWN** to re-engage AUTO. Motors spin again. Sketch prompts
    `Motors spinning again? y/n` → `PASS (4/4)` and summary.

After gate 4 the outputs freeze at neutral; reboot to re-run.

### Optional — exercise the HTTP `/cruise` path

Any time before flipping to AUTO, hit `/cruise` to override the default:

```
curl -X POST http://<boat_ip>/cruise -H 'Content-Type: application/json' -d '{"us":1720}'
```

Or `{"pct": 60}`. Telemetry's `cruise_us` will reflect the new value; the
next `[MODE] MANUAL → AUTO` line shows the value used.

## Telemetry shape

```
GET /telemetry  → {
  "v": "test_27",
  "uptime": 123,
  "mode": "AUTO",
  "cruise_us": 1720,
  "failsafe_ack": false,
  "rc_ever_good": true,
  "rc_age_ms": 16,
  "rudder_us": 1500,
  "esc_us": 1720,
  "ch_throttle": 1003, "ch_rudder": 1500, "ch_swc": 1980,
  "heading": "184.5",
  "bus_v": "14.83", "shunt_ma": "1234"
}
```

## /cruise shape

```
POST /cruise  body {"us": 1720}    → cruise_us = 1720 (must be 1500..1800)
POST /cruise  body {"pct": 50}     → cruise_us = 1500 + (1800-1500)*0.5 = 1650
                                     (right at the floor — 51%+ to engage)
```
