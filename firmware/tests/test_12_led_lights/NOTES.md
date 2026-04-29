# test_12_led_lights — Direct-Drive LED Circuit Bringup

Bench/boat test. Cycles three LED lighting circuits driven directly from ESP32 GPIO pins (no MOSFET, no PCA9685) to confirm wiring is correct on every circuit.

The sketch is one-shot: it walks through each circuit individually for 3 s, then all three together for 3 s, prints PASS, and idles. Reset the board to run again.

---

## Objective

| # | What we're checking |
|---|---------------------|
| 1 | Nav-lights circuit lights when GPIO 18 goes HIGH |
| 2 | Bridge/interior circuit lights when GPIO 19 goes HIGH |
| 3 | Deck/flood circuit lights when GPIO 23 goes HIGH |
| 4 | All three lit simultaneously (no rail brownout, no GPIO crosstalk) |

---

## Hardware required

- ESP32 (USB-powered for bench)
- Three LED circuits, one per GPIO. Each circuit is one or more LEDs in series with a current-limit resistor sized to keep the GPIO at or below 12 mA.
- (Optional) a multimeter to spot-check the per-pin current under load.

---

## Pin choice

| GPIO | Circuit | Why this pin |
|------|---------|--------------|
| 18 | Nav lights | General-purpose, output-capable, free on this build |
| 19 | Bridge/interior | Same |
| 23 | Deck/flood | Same |

Pins were selected from outside the project's reserved set (4, 12-17, 21-22, 25-27, 32-33 — covering iBUS, GPS, I²C, PCA9685, DFPlayer, ESC/servo). 18, 19, 23 are general-purpose, not strapping pins, not input-only, and not used elsewhere in the firmware.

---

## Wiring (per circuit, repeated three times)

```
  ESP32 GPIO ── [R] ── LED(+) ── ... ── LED(-) ── ESP32 GND
```

For a single 3 V LED at ~10 mA from a 3.3 V GPIO: `R = (3.3 − 3.0) / 0.010 = 30 Ω` minimum, but use **220 Ω** for safety and dim-but-visible operation, or **100 Ω** for brighter. White/blue LEDs (Vf ≈ 3.0-3.2 V) want 47-100 Ω.

For multi-LED strings or 5 V strip lights, **do not direct-drive** — add a logic-level N-channel MOSFET (e.g., AO3400, IRLZ44N) between the GPIO and the LED return. Direct drive past 40 mA will damage the GPIO; past 12 mA the pin sags and the LEDs dim/flicker.

---

## Flash and run

1. Open `test_12_led_lights/test_12_led_lights.ino` in Arduino IDE.
2. Board: ESP32 Dev Module. Port: your ESP32's port.
3. Upload.
4. Open Serial Monitor at **115200**.
5. Watch the LEDs (or oscilloscope/multimeter on each pin) as the sketch walks through the sequence.

---

## Expected output

```
========================================
  test_12_led_lights
========================================
Nav lights      GPIO18
Bridge/interior GPIO19
Deck/flood      GPIO23

Step 1: cycling each circuit individually for 3 s.
----------------------------------------
[ON ]  Nav lights        GPIO18
[OFF]  Nav lights        GPIO18
[ON ]  Bridge/interior   GPIO19
[OFF]  Bridge/interior   GPIO19
[ON ]  Deck/flood        GPIO23
[OFF]  Deck/flood        GPIO23

Step 2: all three on together for 3 s.
----------------------------------------
[ON ]  ALL THREE       GPIO18,19,23
[OFF]  ALL THREE       GPIO18,19,23

========================================
  PASS — all three circuits cycled OK
========================================
Reset the board to run the sequence again.
```

The serial PASS prints whether the LEDs lit or not — visual confirmation is the actual pass criterion.

---

## Failure modes

| Symptom | Likely cause |
|---------|--------------|
| One circuit never lights | Resistor open, LED reversed (cathode/anode swapped), or wire off the GPIO. Check continuity. |
| LED is very dim | Resistor too large, OR current draw is higher than 12 mA and the GPIO is sagging. Reduce the LED count on that pin or add a MOSFET. |
| LED flickers when all three are on | Total current exceeds what the ESP32 3.3 V regulator can supply. Move to a MOSFET driver or split power. |
| Wrong LED lights for a given step | Wires swapped between GPIO 18/19/23. Verify physical connection matches the pinout table. |
| Nothing on serial monitor | Wrong baud (must be 115200) or the board didn't reset on upload. |

---

## Pass criteria

- [ ] Nav-lights LED(s) light during the "Nav lights" step
- [ ] Bridge/interior LED(s) light during the "Bridge/interior" step
- [ ] Deck/flood LED(s) light during the "Deck/flood" step
- [ ] All three light together during the "ALL THREE" step
- [ ] No flicker, dimming, or brownout during the all-on step
- [ ] Serial monitor prints `PASS — all three circuits cycled OK`

## Result — pending

(Run the test and fill this in.)
