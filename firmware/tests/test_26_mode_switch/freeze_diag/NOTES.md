# freeze_diag — iBUS "channels frozen" failsafe diagnostic

Stripped-down sketch (iBUS only, no IMU/PCA/WiFi) that runs four 10-second
phases and reports per-phase frame counts, gaps, freeze-trip counts, and
which channels actually changed. Built to localize a regression where
test_17 v2 and test_26 v1 were tripping `>>> FAILSAFE — channels frozen <<<`
constantly despite the boat being controllable, the RX LED solid, and
test_16 reporting a healthy link.

## Background

- test_17 v1 PASSED 2026-05-03 with the channel-freeze detector enabled.
- test_17 v2 (2026-05-04) was marked "pending" in NOTES.md — never tested.
- 2026-05-09: ran test_17 v2 for the first time and the freeze detector
  tripped roughly every second. Same code Ryan ran clean five days earlier
  in v1 (logic bit-identical per `git show`).
- test_16 quality meter showed ~130 fps steady, 0.73% checksum loss, no
  resync growth — the link itself was healthy.
- Removing the bilge MOSFET module did not help. RX LED solid throughout.
- Conclusion before this diag: had to be either a no-jitter TX/RX behavior
  (false trips on operator pauses), brief sub-LED RX brownouts, or a
  parser bug. Diag was written to discriminate.

## Result — 2026-05-10

```
Phase 1: REST (do not touch sticks)
  frames good=1298  bad=1  max_gap=16ms  freezeTrips=0
  changed: ch1:. ch2:. ch3:Y ch4:. ch5:. ch6:. ch7:. ch8:. ch9:. ch10:.
  final  : 1500 1500 1003 1500 1124 1000 1500 1500 1500 1500

Phase 2: WIGGLE RUDDER (CH1, right-stick H)
  frames good=1295  bad=4  max_gap=16ms  freezeTrips=0
  changed: ch1:Y ch2:Y ch3:Y ch4:. ch5:. ch6:. ch7:. ch8:. ch9:. ch10:.
  final  : 1499 1501 1003 1500 1124 1000 1500 1500 1500 1500

Phase 3: WIGGLE THROTTLE (CH3, left-stick V)
  frames good=1292  bad=7  max_gap=16ms  freezeTrips=0
  changed: ch1:Y ch2:. ch3:Y ch4:Y ch5:. ch6:. ch7:. ch8:. ch9:. ch10:.
  final  : 1500 1500 1236 1500 1124 1000 1500 1500 1500 1500

Phase 4: REST again (do not touch sticks)
  frames good=1295  bad=2  max_gap=15ms  freezeTrips=4
  changed: ch1:Y ch2:. ch3:. ch4:. ch5:. ch6:. ch7:. ch8:. ch9:. ch10:.
  final  : 1501 1500 1000 1500 1124 1000 1500 1500 1500 1500
```

## Interpretation

- **Receiver / link is healthy.** ~130 fps every phase, max gap 15-16 ms
  (one missed frame), 1-7 bad frames per 10 s window. No brownouts, no
  burst losses, no parser desync.
- **No false trips during deliberate movement** (phases 2 & 3, freezeTrips=0).
  Rules out the "frame-handling bug or sub-LED RF dropouts" hypothesis.
- **Phase 4 is the smoking gun.** Sticks truly at rest for 10 s →
  freezeTrips=4. The receiver is not producing reliable per-frame jitter on
  idle channels. Channels stayed bit-identical for windows >500 ms,
  tripping the detector.
- **Phase 1 freezeTrips=0 with ch3:Y** initially looked anomalous (Ryan
  reported he wasn't touching the throttle). Best explanation: the FS-i6X
  gimbal pots produce occasional sparse electrical noise on individual
  channels — sometimes enough to keep the timer reset, sometimes not.
  Phase 4 had the same conditions but barely any jitter, so trips fired.
  The behavior is unreliable, not zero.

## Hypothesis confirmed

The "channels frozen (TX off?)" detector in test_17 / test_26 trips on
operator stillness because this RX/TX combo produces near-zero per-frame
gimbal jitter on idle sticks. Five days ago when test_17 v1 PASSED, the
operator was actively manipulating sticks throughout and never paused
long enough to expose the bug. v2 was never tested until 2026-05-09.

## Fix applied to test_26

Removed the `FROZEN_TIMEOUT_MS` check from `framesOK()` in
`test_26_mode_switch.ino` (2026-05-10). The remaining no-frame timeout
(`FAILSAFE_MS = 1500`) catches actual receiver power loss, which is what
matters for safety. The freeze detector's only unique value was catching
the rare case of "RX still sending frames but TX is off and outputting
preset failsafe values" — and a boat that legitimately runs straight for
long stretches makes the generic detector unusable.

## Recommendation for future tests

If a future sketch wants to detect the "TX off, RX sending failsafe values"
case specifically, do NOT use a generic "any channel unchanged for N ms"
check. Instead check for the exact failsafe pattern (typically all
channels at preset values like 1500), held for 5+ seconds. Or just rely
on the no-frame timeout — most marine use cases don't need belt and
suspenders here.

## Files touched

- `test_26_mode_switch/test_26_mode_switch.ino` — `framesOK()` simplified
- `test_26_mode_switch/freeze_diag/freeze_diag.ino` — this diagnostic
- `test_26_mode_switch/freeze_diag/NOTES.md` — these notes
