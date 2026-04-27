# test_11_dfplayer — DFPlayer Mini Bringup

Bench test. Confirms a DFPlayer Mini MP3 module boots, talks UART at 9600 baud, and plays track 1 on startup and again every 20 seconds.

Audio path on the boat is DFPlayer → mini stereo amp → 2 speakers. The amp and speakers are passive on the data side, so this test only exercises the UART control link and the DFPlayer's own DAC. If track 1 plays cleanly, the digital side is good; tone/volume tuning is a separate exercise.

---

## Objective

| # | What we're checking |
|---|---------------------|
| 1 | `dfPlayer.begin()` returns true (module ACKs over UART) |
| 2 | Track 1 plays — auditory verification |

After PASS 2/2, the sketch replays track 1 every 20 s and prints any DFPlayer events (track-finished, errors) as they arrive on the event stream.

---

## Hardware required

- ESP32 (USB-powered for bench)
- DFPlayer Mini module
- microSD card, FAT32, with `0001.mp3` placed in root **first** (DFPlayer indexes by file copy order, not filename)
- Mini stereo amp + 2 speakers, OR a single speaker wired direct to SPK_1/SPK_2 (DFPlayer has a built-in 3W mono amp on those pins)
- 1kΩ resistor (recommended) in series with ESP32-TX → DFPlayer-RX
- DFRobotDFPlayerMini library installed

> The DFPlayer Mini is a 5V part. **Power VCC from 5V, not 3.3V** — at 3.3V the module either fails to boot or boots in a degraded state where `begin()` randomly fails. The UART logic is 5V-tolerant on its RX, but a 1k series resistor between the ESP32's 3.3V TX and the DFPlayer's RX is cheap insurance.

---

## Library required

Install via Arduino IDE → Library Manager:

> **DFRobotDFPlayerMini** by DFRobot

(There are several DFPlayer libraries; this is the one the main firmware uses, so match it.)

> The bench test uses ESP32 **HardwareSerial(2)** on custom pins (25/26) — no `SoftwareSerial` library needed. The main firmware (`legend_cutter/audio.cpp`) uses SoftwareSerial because at runtime UART1 is iBUS and UART2 is GPS, leaving no hardware UART free. If you ever build the main firmware and hit `SoftwareSerial.h: No such file`, install **EspSoftwareSerial** by Peter Lerup from Library Manager.

---

## Wiring

| DFPlayer pin | Bench pin | Notes |
|--------------|-----------|-------|
| VCC | **5V** | NOT 3.3V — module needs 5V to boot reliably |
| GND | GND | Tie ESP32 GND, DFPlayer GND, and amp GND together |
| RX  | **GPIO 25** (ESP32 TX) | Put a 1kΩ series resistor in this line |
| TX  | **GPIO 26** (ESP32 RX) | |
| SPK_1 | amp L input | Or direct to one terminal of an 8Ω speaker for mono |
| SPK_2 | amp R input | Or other terminal of the speaker |
| GND (amp) | shared GND | Floating amp GND = no audio + buzz |

> Pinout matches `legend_cutter/config.h` (`DFPLAYER_TX_PIN=25`, `DFPLAYER_RX_PIN=26` — ESP32-side: RX=26, TX=25), so the boat firmware can reuse this wiring as-is.

---

## microSD prep

DFPlayer Mini does NOT play files by their numeric filename — it plays the Nth file in the FAT directory entry order. To make `dfPlayer.play(1)` actually play `0001.mp3`:

1. Format the card FAT32 (clean format, not quick-erase on top of a previous filesystem).
2. Copy `0001.mp3` to the root **first**, by itself.
3. Copy any other tracks (`0002.mp3`, `0003.mp3`, …) afterward, in numerical order.

If track 1 plays the wrong file, reformat and re-copy in order. There is no other fix.

---

## Flash and run

1. Open `test_11_dfplayer/test_11_dfplayer.ino` in Arduino IDE.
2. Board: ESP32 Dev Module. Port: your ESP32's port.
3. Upload.
4. Open Serial Monitor at **115200**.
5. Listen for track 1 from the speakers within ~1 s of the `PASS (1/2)` line.

---

## Expected output (happy path)

```
========================================
  test_11_dfplayer
========================================
HardwareSerial(2)  baud=9600  RX=GPIO26 (← DFP TX)  TX=GPIO25 (→ DFP RX)
Volume=20/30  test track=1  replay every 20s

Step 1: opening UART and calling dfPlayer.begin()...
(begin() can take ~3 s on a cold module while the SD spins up)
----------------------------------------
PASS (1/2): DFPlayer responded (after 1820 ms).
Step 2: playing track 1 — listen for audio out of the speakers.
----------------------------------------
[t=    1s]  play #1  track=1

PASS (2/2) is auditory: confirm you hear track 1 from the speakers.
If silent, check: SD inserted, 0001.mp3 in root, amp powered, volume.
  [DF] SD card online
  [DF] track 1 finished
[t=   21s]  play #2  track=1
  [DF] track 1 finished
[t=   41s]  play #3  track=1
...
```

---

## Failure modes the sketch surfaces

| Symptom | Meaning |
|---------|---------|
| `FAIL (1/2): DFPlayer did not respond to begin().` | Module didn't ACK. Most common cause: VCC on 3.3V instead of 5V. Next: TX/RX swapped, or 1k series resistor open-circuit. |
| `[DF] error code 1 (busy / card not found)` | SD missing, not seated, or formatted exFAT. Reformat FAT32. |
| `[DF] error code 6 (file index out of range — track missing on SD)` | No track 1 on the card. Either nothing copied, or `0001.mp3` was not the first file copied. |
| Track plays once but never repeats | Loop logic stuck — check `lastPlayMs` reset path (shouldn't happen unless `millis()` rolls, which takes 49 days). |
| Wrong file plays on track 1 | SD copy order wrong. Reformat, copy `0001.mp3` first by itself. |
| Audible buzz but no music | Amp GND not tied to ESP32/DFPlayer GND. Tie all GND to one point. |
| Audio crackles when motors run | Power rail noise. Run DFPlayer off a separate buck or add a bulk cap. Not relevant on bench. |

---

## Pass criteria

- [ ] `PASS (1/2)` — `begin()` succeeds
- [ ] `PASS (2/2)` — track 1 audible on the speakers within ~1 s of boot
- [ ] (visual) `play #N` line prints every 20 s and audio replays
- [ ] (visual) `[DF] track 1 finished` events appear between replays (if track is < 20 s long)

## Result — pending

(Run the test and fill this in.)
