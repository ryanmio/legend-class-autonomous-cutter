# test_05_deckgun — Deck Gun Pan Servo

## Hardware
- Pan: continuous rotation servo on GPIO 25
- Tilt: **dropped** — see below

## Bench test result
Pan working as of 2026-03-19. Slow traversal left/right confirmed.

## Tuning notes
- Continuous servo dead-band is not symmetric: right (below 1500) needs less offset than left (above 1500) to get matched speeds
- Settled on `PAN_SLOW_RIGHT_US 1470` / `PAN_SLOW_LEFT_US 1560` for roughly matched slow speed
- If the servo creeps at stop, trim `PAN_STOP_US` ±5 µs until it holds
- This is about as slow as the servo will go — the dead-band edge is the hard floor

## Tilt dropped
The 2.1g micro servo on the barrel tilt was killed during testing. Root cause: too much friction in the pushrod linkage at the scale modeled, combined with jumping the pulse range too wide (±167 µs / ~30°) before confirming mechanical clearance. The servo stalled, ran hot, and died.

Tilt barrel elevation is not worth revisiting unless the linkage is redesigned with much less friction or a more robust servo (9g minimum) is fitted. For now the deck gun is pan-only.

## App control (future)
Pan maps to a joystick/slider: center = 1500 (stop), left/right = speed in that direction.
No position feedback — operator drives it manually, releases to stop.
