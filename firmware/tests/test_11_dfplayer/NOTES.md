# test_11_dfplayer — DF1201S (DFPlayer Pro) Bringup

Bench test. Confirms a DFRobot **DF1201S** (a.k.a. DFPlayer Pro, SKU DFR0768) MP3 module boots, talks AT-protocol UART at 115200 baud, and plays file 1 on startup and again every 20 seconds.

Audio path on the boat is DF1201S → mini stereo amp → 2 speakers. The DF1201S also has an on-board 8Ω/3W amp (controlled by `enableAMP()` / `disableAMP()`) — feed line-out to the external amp, or wire a single speaker directly to SPK+/SPK-.

> ⚠ The DF1201S is **not the same module** as the cheap DFPlayer Mini.
>
> | | DFPlayer Mini | DF1201S / DFPlayer Pro |
> |---|---|---|
> | Baud | 9600 | **115200** |
> | Protocol | binary frames `0x7E ... 0xEF` | AT command strings |
> | Library | `DFRobotDFPlayerMini` | **`DFRobot_DF1201S`** |
> | Internal flash | no, SD only | yes, ships with sample track at index 1 |
>
> Pointing the wrong driver at the wrong module produces no audio and a fast garbage stream on the RX line — confirmed the hard way during this bringup.

---

## Objective

| # | What we're checking |
|---|---------------------|
| 1 | `DF1201S.begin()` returns true (module ACKs over UART) |
| 2 | File 1 plays — auditory verification |

After PASS 2/2, the sketch replays file 1 every 20 s.

---

## Hardware required

- ESP32 (USB-powered for bench)
- DFRobot DF1201S / DFPlayer Pro module
- Mini stereo amp + 2 speakers, OR a single 8Ω speaker direct to SPK+/SPK-
- DFRobot_DF1201S library installed
- (optional) microSD card. The DF1201S has internal flash with a built-in track at index 1, so the bench test works with no card inserted.

> The DF1201S is a 3.3-5V part — either rail works. 5V gives full amp output.

---

## Library required

Install via Arduino IDE → Library Manager:

> **DFRobot_DF1201S** by DFRobot

(Do **not** install or use `DFRobotDFPlayerMini` — that's for the older binary-protocol Mini and will not communicate with this module.)

> The bench test uses ESP32 **HardwareSerial(2)** on custom pins (25/26) — no `SoftwareSerial` library needed. If `legend_cutter/audio.cpp` is later refactored for the DF1201S it will likely also use HardwareSerial, since 115200 baud SoftwareSerial on ESP32 is unreliable.

---

## Wiring

| DF1201S pin | Bench pin | Notes |
|-------------|-----------|-------|
| VIN | **3.3V** | 5V works only with a level shifter on TX. 3.3V is the safe default and matches the Fitz reference. |
| GND | GND | Tie ESP32 GND, DF1201S GND, and amp GND together |
| RX  | **GPIO 26** (ESP32 TX) | |
| TX  | **GPIO 27** (ESP32 RX) | |
| SPK+ | amp L+ input | Or direct to one terminal of an 8Ω speaker |
| SPK- | amp L- input | Or other terminal of the speaker |
| GND (amp) | shared GND | Floating amp GND = no audio + buzz |

> **Pinout differs from `legend_cutter/config.h` and from the original test_11 plan.** config.h was written when the project still expected a DFPlayer Mini and used GPIO 25/26. Bench bringup against the Fitz reference (the working sister project) revealed that 25/26 with 5V VIN does not work — the proven combination is **GPIO 26/27 with 3.3V VIN**. config.h needs to be updated to match before the boat firmware can drive this module.

---

## microSD prep

Optional. The DF1201S ships with a working sample at index 1 in internal flash, which is what this test plays. To play files from SD:

1. Format the card FAT32.
2. Copy MP3s to the root or a subfolder.
3. Use `playFileNum(N)` to play by index, or `playFile("/path/file.mp3")` to play by path.

(The DFPlayer-Mini-era "must copy 0001.mp3 first" rule does **not** apply to the DF1201S — this module indexes by FAT entry but the API also supports filename paths.)

---

## Flash and run

1. Open `test_11_dfplayer/test_11_dfplayer.ino` in Arduino IDE.
2. Board: ESP32 Dev Module. Port: your ESP32's port.
3. Upload.
4. Open Serial Monitor at **115200**.
5. Listen for file 1 from the speakers within ~3 s of `PASS (1/2)` (the 2 s `switchFunction(MUSIC)` settle delay accounts for most of the gap).

---

## Expected output (happy path)

```
========================================
  test_11_dfplayer (DF1201S / DFPlayer Pro)
========================================
HardwareSerial(2)  baud=115200  RX=GPIO27 (← DF1201S TX)  TX=GPIO26 (→ DF1201S RX)
Volume=20/30  test track=1  replay every 20s

Step 1: opening UART and calling DF1201S.begin()...
(begin() can take 1-3 s)
----------------------------------------
PASS (1/2): DF1201S responded (after 820 ms).
Step 2: playing file #1 — listen for audio out of the speakers.
----------------------------------------
[t=    3s]  play #1  file=1

PASS (2/2) is auditory: confirm you hear file 1 from the speakers.
If silent, check: amp powered, volume, on-board AMP enabled (it is).
[t=   23s]  play #2  file=1
[t=   43s]  play #3  file=1
...
```

---

## Failure modes the sketch surfaces

| Symptom | Meaning |
|---------|---------|
| `FAIL (1/2): DF1201S did not respond to begin() within 5 s.` | Module didn't ACK. Check VCC, GND continuity, TX/RX direction, and that you really are on 115200 baud (not 9600). |
| `begin()` passes but no audio | Volume too low, on-board AMP disabled, or speakers wired to SPK pins on a module configured for line-out only. Try `DF1201S.enableAMP()` (already in the sketch). |
| Fast unreadable stream of bytes on the serial monitor | You're using the DFPlayer Mini library/baud against a DF1201S (or vice versa). Re-check the includes and `DFP_BAUD`. |
| File 1 plays but won't replay | `setPlayMode(SINGLE)` not in effect — module went into "stop after first track" but `playFileNum` should re-trigger anyway. Confirm `playFileNum(1)` is being called every 20 s in the loop. |
| Audible buzz but no music | Amp GND not tied to ESP32/DF1201S GND. Tie all GND to one point. |

---

## Pass criteria

- [ ] `PASS (1/2)` — `begin()` succeeds
- [ ] `PASS (2/2)` — file 1 audible on the speakers within ~3 s of boot
- [ ] (visual) `play #N` line prints every 20 s and audio replays

## Result — pending

(Run the test and fill this in.)

---

## Bringup history

This bringup took three false starts before working:
1. First built against the **DFPlayer Mini library at 9600 baud** — `begin()` always timed out. Tried both pin orderings, multimeter-confirmed power, confirmed audio-via-play-button. Nothing worked.
2. User pointed at the [DFRobot_DF1201S repo](https://github.com/DFRobot/DFRobot_DF1201S) — the module is actually a **DF1201S (DFPlayer Pro)**, not a Mini. Library swapped to `DFRobot_DF1201S` at 115200 baud. `begin()` *still* failed.
3. User pointed at working code at `EdmundFitzgeraldController/firmware/dfplayer_diagnostic` (the "Fitz" reference). Three differences from our setup explained it:
   - **Pins**: Fitz uses GPIO **26/27** (TX/RX). Our wires were on 25/26.
   - **VIN**: Fitz uses **3.3V**. We had 5V — which needs a level shifter on the ESP32-3.3V TX → DF1201S RX line, or `begin()` times out.
   - **Settle delay**: Fitz waits **1000 ms** after `Serial2.begin` before `DF1201S.begin()`. We had 200 ms.

   Aligning to all three made it work.

The lessons:
- When `begin()` fails with confirmed power and confirmed audio, suspect a module identification mismatch before chasing wiring further — but
- "Same module" doesn't mean same pinout/voltage. If a sister project has the device working, port their config exactly before improvising.
