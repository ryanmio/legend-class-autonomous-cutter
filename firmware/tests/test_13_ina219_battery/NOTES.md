# test_13_ina219_battery — INA219 Bringup + 4S LiPo Health

Bench/boat test. Confirms the INA219 current/voltage sensor is alive on I2C at 0x41 and reads sensible values for a 4S LiPo battery, then streams a 1 Hz telemetry line with pack voltage, per-cell voltage, current, power, and an estimated state of charge.

The same sensor and address are used by `legend_cutter/battery.cpp` in the main firmware (Adafruit INA219 library, `setCalibration_32V_2A()`).

---

## Objective

| # | What we're checking |
|---|---------------------|
| 1 | INA219 detected on I2C at 0x41 |
| 2 | Bus voltage in 4S LiPo range (10.0-17.5 V) |
| 3 | Current reading is finite and not pinned to a rail |

After all three pass, the sketch streams battery telemetry at 1 Hz so you can verify voltage sags under load and recovers when the load drops.

---

## Hardware required

- ESP32 (USB-powered for bench, or boat-powered)
- INA219 breakout (Adafruit or clone), A0 jumper bridged so it answers at **0x41** (not 0x40 — that's the PCA9685)
- 4S LiPo battery in some state of charge
- Optional: a load (small motor, light bulb, resistor) on VIN- so you can watch current move
- I2C pull-ups: most breakouts include 4.7-10 kΩ pull-ups already; if not, add them on SDA/SCL

---

## Library required

Install via Arduino IDE → Library Manager:

> **Adafruit INA219** by Adafruit

Same library the main firmware uses, so behavior matches once the test passes.

---

## Wiring

| INA219 pin | Connect to | Notes |
|-----------|------------|-------|
| VCC | ESP32 3.3V (or 5V) | INA219 logic is tolerant; pick whichever matches your I2C bus level |
| GND | ESP32 GND | Tie the battery negative to this same GND for a clean shunt reading |
| SDA | ESP32 GPIO 21 | shared I2C bus with PCA9685 (0x40) and ICM-20948 (0x68) |
| SCL | ESP32 GPIO 22 | |
| **VIN+** | Battery (+) terminal | high side of the shunt |
| **VIN-** | Load (+) — ESC, BEC input, etc. | after the shunt — current flows VIN+ → VIN- through the 0.1 Ω shunt |

> Address jumper: A0 must be bridged to put the INA219 at **0x41**. With no jumpers it defaults to 0x40, which collides with the PCA9685.

---

## Calibration

The sketch calls `setCalibration_32V_2A()`:
- Bus voltage range: 32 V (covers 4S LiPo's 16.8 V max)
- Current range: ±3.2 A with the breakout's stock 0.1 Ω shunt
- LSB: ~100 µA on current, 4 mV on bus voltage

**For measuring remaining battery percent (this project's actual use case): the INA219 with default calibration is sufficient.** Voltage measurement is independent of the shunt and stays accurate at any current — 0-32 V clean across the full 4S range. State-of-charge estimation is voltage-based (4.20 → 3.30 V/cell lookup), which is the standard approach for RC/small-craft battery monitors.

What the default calibration *can't* do:
- Read instantaneous current above ~3.2 A (clips at the rail).
- Coulomb-count under motor load (current integrand is wrong when the reading saturates).

If you ever want true motor-current telemetry or Ah accounting (you don't, for SoC%), you'd need a lower-value shunt (e.g., 0.01 Ω with manual `setCalibration_*`) or a higher-range chip like the **INA226** / **INA228**. That's a future-feature decision, not a blocker for the battery monitor.

Caveats of voltage-based SoC to keep in mind:
- Voltage sags under load — a momentary 30 A pull briefly reads low. Average over a second or only trigger RTH at idle.
- LiPo discharge curve is flat between ~50-30% SoC, so percentage gets fuzzy in that band. The RTH/ALARM voltage thresholds in `config.h` are still meaningful — they sit on the steep part of the curve.

---

## Flash and run

1. Open `test_13_ina219_battery/test_13_ina219_battery.ino` in Arduino IDE.
2. Board: ESP32 Dev Module. Port: your ESP32's port.
3. Upload.
4. Open Serial Monitor at **115200**.
5. Connect the 4S LiPo through the INA219 (VIN+ to battery+, VIN- to load).

---

## Expected output (happy path, freshly charged 4S)

```
========================================
  test_13_ina219_battery
========================================
Target: INA219 @ 0x41 (SDA=GPIO21, SCL=GPIO22)
Battery: 4S LiPo (16.8 V full, 14.8 V nominal, 13.0 V RTH)

--- I2C bus scan (SDA=GPIO21, SCL=GPIO22) ---
  0x40  <- PCA9685 (servo driver)
  0x41  <- INA219 (this test)
  0x68  <- ICM-20948 (IMU)
  0x70  <- PCA9685 all-call

PASS (1/3): INA219 detected and initialized (after 120 ms).
Step 2: confirming bus voltage in 4S LiPo range (10.0-17.5 V)...
----------------------------------------
PASS (2/3): pack voltage 16.62 V is in 4S LiPo range.
Step 3: confirming current reading is finite...
----------------------------------------
PASS (3/3): current 0.014 A reads cleanly.

========================================
  ALL TESTS PASSED
========================================
Streaming 1 Hz battery telemetry. Ctrl-C / reset to stop.
V_bus=16.62V  V_shunt= +1.40mV  V_pack=16.62V  V/cell=4.16V  I= +0.01A  P= +0.23W  SoC= 96%  [FULL]
V_bus=16.61V  V_shunt= +1.40mV  V_pack=16.61V  V/cell=4.15V  I= +0.01A  P= +0.23W  SoC= 95%  [FULL]
...
```

State labels (from pack voltage):
- **FULL**: ≥16.75 V
- **good**: 14.8-16.75 V
- **ok**: 13.6-14.8 V
- **ALARM (warn)**: 13.0-13.6 V — matches `BATTERY_ALARM_VOLTAGE`
- **RTH (land now)**: 12.0-13.0 V — matches `BATTERY_RTH_VOLTAGE`
- **DEAD (damaged?)**: <12.0 V — pack is below 3.0 V/cell

---

## Failure modes the sketch surfaces

| Symptom | Meaning |
|---------|---------|
| `FAIL (1/3): no I2C device at 0x41` | INA219 unpowered, GND missing, SDA/SCL swapped, or A0 jumper not bridged (defaults to 0x40 → collides with PCA9685). |
| `FAIL (1/3): I2C found 0x41 but ina219.begin() rejected it` | Something else is at 0x41. Unlikely on this build. |
| `V_pack` stuck near 0 V or rails | Battery not connected to VIN+, or VIN+/VIN- swapped (current flows the wrong way; voltage reading will look odd). |
| `V_pack` drifts 0.5+ V from a known-good multimeter reading | Shunt resistor wrong value (some clones ship with 0.01 Ω instead of 0.1 Ω) — recalibrate via `setCalibration_*`. |
| Current saturates at ~3.2 A under load | Default 32V/2A range maxed out. Expected for boat motor draw — use a lower shunt or INA226 in production firmware. |
| Negative current at idle | VIN+/VIN- swapped, or the load is feeding current back (regen). Swap VIN+/VIN- if at idle. |

---

## Pass criteria

- [x] `PASS (1/3)` — INA219 detected
- [x] `PASS (2/3)` — pack voltage in 10.0-17.5 V band
- [x] `PASS (3/3)` — current reading finite (reads 0.00 A — shunt jumpered)
- [x] (visual) Pack voltage matches multimeter at battery terminals (15.00 V on chip vs 15.12 V on multimeter — within 0.12 V)
- [ ] (visual) Voltage sags under load and recovers when load drops — not exercised; not required for SoC%

## Result — 2026-04-29

**Status: PASS (voltage-only configuration with shunt jumpered).**

Stable readings on a 4S LiPo at ~50% SoC:
```
V_bus=15.00V  V_shunt= +0.05mV  V_pack=15.00V  V/cell=3.75V  I=-0.00A  P=-0.00W  SoC=50%  [good]
V_bus=15.04V  V_shunt= +0.02mV  V_pack=15.04V  V/cell=3.76V  I=-0.00A  P=-0.00W  SoC=52%  [good]
```

### Final wiring on the boat

- INA219 VCC → ESP32 3.3 V
- INA219 GND → boat negative
- INA219 SDA / SCL → ESP32 GPIO 21 / 22
- INA219 VIN+ → battery (+) (high side)
- **INA219 VIN- → jumpered directly to VIN+ on the breakout** (shorts the on-board shunt; tying VIN- to battery voltage is what makes `getBusVoltage_V()` read accurately)
- INA219 VIN- screw terminal → nothing connected externally

### Bringup notes — what went wrong and how we ended up here

1. **First attempt** wired VIN- back to the negative wago that returns to battery (-). That created a direct short across the 0.1 Ω shunt: battery+ → VIN+ → shunt → VIN- → battery-. The boat was briefly powered on in this state. The on-board surface-mount shunt resistor very likely burned open during this short — current readings have been pinned at the +3.20 A saturation rail ever since.

2. **Second attempt (intermediate, wrong)** disconnected VIN- entirely, leaving the screw terminal floating. V_bus *appeared* to read a believable low-4S number (~13.6 V), so the test was initially marked PASS. **It was wrong.** The INA219 measures bus voltage at the VIN- pin (per the TI datasheet), so a floating VIN- gives a meaningless reading that just happened to look plausible. Multimeter at the battery showed 15.12 V at the same time the chip reported 13.6 V — a ~1.5 V discrepancy proved the floating reading was junk.

3. **Final fix** jumpered VIN+ directly to VIN- on the breakout itself. This ties VIN- to battery voltage, so `getBusVoltage_V()` now reads the actual battery (15.00 V on the chip vs 15.12 V on the multimeter — within 0.12 V, the expected accuracy of an INA219 doing low-current bus voltage). V_shunt drops to noise (+0.05 mV) and current reports 0.00 A, which is honest because there's no path for current to flow.

The lesson for future-anyone: with the on-board shunt damaged, "leave VIN- disconnected" is **not** equivalent to "jumper VIN+ to VIN-". The first floats the bus measurement; the second forces it to battery voltage. Always do the jumper.

### What this means for the runtime firmware

`legend_cutter/battery.cpp`:
- `voltageV = getBusVoltage_V() + getShuntVoltage_mV()/1000`: works correctly. With the jumper, V_bus is battery voltage and V_shunt is ~0, so voltageV ≈ battery voltage. Voltage drives `lowVoltage` and `criticalVoltage` flags via `BATTERY_ALARM_VOLTAGE` / `BATTERY_RTH_VOLTAGE`. ✓
- `currentA` and `powerW`: report 0.00 A / 0.00 W (no current can flow through a jumper-shorted shunt). Not consumed by the runtime alarm/RTH logic, so harmless — but anything that *does* read them downstream (telemetry, logging, future features) will see zeros instead of real values.

### Followups (if anyone ever wants real current measurement)

- Replace the breakout (the on-board shunt is toast and now bypassed by the jumper).
- Wire VIN+ ← battery (+) main switch output, VIN- → 14 V positive distribution to ESC + BEC inputs (i.e. cut the 14 V positive bus and put the INA219 in series with it). NOT to the 5 V wago — that's downstream of a BEC and won't see real battery current.
- Default `setCalibration_32V_2A()` will still saturate above ~3 A. For real motor-current logging, swap to a lower-value shunt (e.g., 0.01 Ω with manual `setCalibration_*` numbers) or upgrade to an INA226 / INA228.
- None of that is needed for battery percent.
