# test_19_anchor_winch — Knob → Forward Anchor Winch

Controls a continuous-rotation servo near the bow used to raise and lower the anchor.

---

## Behavior

The knob (CH5, iBUS index 4) commands the winch in binary mode — speed is fixed, not proportional. This prevents accidental slow-creep if the knob drifts slightly off center.

| Knob position | µs range | Winch command |
|---|---|---|
| Far left (hard stop) | ≤ 1050 µs | Lower anchor (slow) |
| Far right (hard stop) | ≥ 1950 µs | Raise anchor (slow) |
| Anywhere between | 1051–1949 µs | Stop (neutral) |

The wide dead zone (1051–1949) is intentional — only the hard extremes activate the winch.

### Speed tuning

`WINCH_LOWER_US = 1350` and `WINCH_RAISE_US = 1650` produce ~40% speed on a typical FS90R-class continuous servo. Move each value further from 1500 (toward 1000 or 2000) to increase speed.

### Failsafe

The winch stops immediately on iBUS loss (no-frame or channel-freeze). Never runs unattended.

---

## Hardware

| Item | Connection |
|---|---|
| iBUS receiver | GPIO 16 (1 kΩ + 2 kΩ divider, same as all iBUS tests) |
| Winch servo signal | PCA9685 ch9 (one above gun pan servo on ch8) |
| PCA9685 SDA/SCL | GPIO 21 / 22 |

---

## Pass criteria

- [ ] iBUS frames arrive within ~5 s of TX power-on
- [ ] Knob in center/mid position: winch holds at 1500 µs (stopped)
- [ ] Knob hard-left (≤ 1050 µs): winch runs, anchor chain moves in lower direction
- [ ] Knob hard-right (≥ 1950 µs): winch runs, anchor chain moves in raise direction
- [ ] Releasing knob to mid: winch stops cleanly
- [ ] TX off: winch stops within 500 ms (failsafe)
- [ ] TX back on: failsafe clears, knob resumes control

---

## Result

*(fill in after test run)*
