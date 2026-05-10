# test_27_rc_failsafe — Notes

## Gates

| Gate | Description | Result |
|------|-------------|--------|
| GATE 1/4 | Boot, SwA UP, sticks safe → MODE_MANUAL detected | PASS |
| GATE 2/4 | flip SwA DOWN → MODE_AUTO at 1660 µs, motors spin (operator y) | PASS |
| GATE 3/4 | TX off → CH6 guard trips, MODE_FAILSAFE, motors neutral (operator y) | PASS |
| GATE 4/4 | TX on with SwA=AUTO → ACK_REQUIRED → flip UP to ack → flip DOWN → AUTO re-engages, motors spin (operator y) | PASS |

## Result: PASS (2026-05-10)

First firmware-detected RC failsafe. Guard-based detection on CH6 (SwD,
configured in TX failsafe menu to ~2000 µs on signal loss) fired within
~500 ms of TX off, putting the boat into MODE_FAILSAFE with both ESCs
and rudder neutralized. Sticky-ack recovery exercised end-to-end:
firmware refused to re-engage AUTO until SwA was flipped to MANUAL,
then accepted the next AUTO request normally.

The 3 s no-frame timeout (defense-in-depth secondary) was not the path
that fired this test — frames kept streaming throughout TX-off
(FS-iA10B holds-last). The guard trigger is what made this work.

## What this test proves (NEW vs everything before)

1. **Canonical `Mode` enum** (`MODE_MANUAL`, `MODE_AUTO`, `MODE_FAILSAFE`) wired
   end-to-end through the state machine and applied to outputs by mode, not
   by gate. Same enum the production firmware will inherit.
2. **3 s no-frame RC failsafe** — outputs forced neutral, mode → FAILSAFE.
   Replaces the test_26 1.5 s "halt the test" pattern with a real mode that
   the test continues to operate in.
3. **Sticky ACK** — once FAILSAFE trips, firmware refuses to re-engage AUTO
   even after RC frames return. Operator must flip SwA to MANUAL to
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

**Channel map (locked 2026-05-10):**
- CH6 (idx 5) = SwD failsafe guard. Sits up (~1000 µs). Receiver outputs
  ~2000 µs from its stored failsafe value when TX is gone.
- CH7 (idx 6) = SwA mode switch. Up = MANUAL, down = AUTO.

**PRIMARY trigger** (the actual TX-loss detector for FS-iA10B):
`ch[5] > 1500 µs` sustained for 500 ms → MODE_FAILSAFE.

**SECONDARY trigger** (defense-in-depth for receivers that genuinely go
silent on signal loss): `millis() - lastFrameMs >= 3000` → MODE_FAILSAFE.
Doesn't fire on FS-iA10B with current config — it holds-last — but should
still be honored if a future hardware change makes this case real.

**Behavior on either trigger:** mode → FAILSAFE, `failsafeAckRequired = true`,
outputs neutral (rudder + ESCs).

**Recovery (sticky ack):** even after the guard clears or frames return,
mode stays FAILSAFE until firmware observes a clean `swcInManual()`
(`ch[6] < 1450 µs`, SwA up). On that observation, `failsafeAckRequired`
clears, mode → MANUAL. Operator can then flip SwA DOWN to re-arm AUTO
normally.

**Override direction is one-way:** firmware can only *downgrade* the
operator's mode request (AUTO → MANUAL/FAILSAFE), never *upgrade*.
SwA=MANUAL is always honored; SwA=AUTO is conditional on (a) guard clear,
(b) frames good, (c) no ack pending, (d) cruise µs valid.

**Pre-RC state:** while `ibusEverGood == false` (boot before any iBUS frame),
outputs are forced neutral regardless of mode value. This is *not* the
FAILSAFE state — it's a separate startup safe-hold.

**SwD operator rule:** SwD must remain at the up position throughout
operation. The receiver outputs the stored failsafe value (~2000 µs) on
CH6 only during signal loss; deliberately flicking SwD down would also
trip MODE_FAILSAFE. This is by design.

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

The sketch defaults cruise to 1660 µs (just above test_26's 1653 µs spin
point — quiet on the bench). `POST /cruise` is exercised separately by
hitting it from curl/the app at any time during the run; the value takes
effect on the next AUTO entry.

1. Props OFF or boat firmly secured.
2. Copy `secrets.h.example` → `secrets.h` and fill in WiFi credentials.
3. Flash. Open Serial @ 115200.
4. Power up boat, wait through 3 s ESC arming.
5. TX on, sticks safe, **SwA UP**, **SwD UP** → `PASS (1/4): MANUAL detected`.
6. **Flip SwA DOWN.** Mode → AUTO at 1660 µs, motors spin. Sketch prints
   `AUTO engaged. Verify motors spinning, then turn off the TX.`
7. **Turn off the TX.** Within 500 ms: guard trips, ESCs neutral, sketch
   prints `[FAILSAFE] guard tripped — ch[5]=2000 sustained 500 ms...`
   then prompts `Did motors spin in AUTO and then STOP after TX off? y/n`.
   `y` → `PASS (2/4) + (3/4)`, `n` → FAIL.
8. **Turn TX back on with SwA still in the AUTO position.** Sketch prints
   `[FAILSAFE] frames restored but ACK_REQUIRED`. Mode stays FAILSAFE.
9. **Flip SwA UP** to acknowledge. `[FAILSAFE] cleared. mode=MANUAL`.
10. **Flip SwA DOWN** to re-engage AUTO. Motors spin again. Sketch prompts
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
  "ch_throttle": 1003, "ch_rudder": 1500, "ch_mode": 2000, "ch_guard": 1000,
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
