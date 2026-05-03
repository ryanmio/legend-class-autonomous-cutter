# test_10_bn220_gps — BN-220 GPS Bringup

Bench test. Confirms a BN-220 GPS module is wired correctly, talking NMEA at 9600 baud, and acquires a position fix when given a sky view.

The BN-220 uses a u-blox M8 chipset internally. Ships defaulted to 9600 baud, 1 Hz, GPS+GLONASS, standard NMEA output.

---

## Objective

| # | What we're checking |
|---|---------------------|
| 1 | Bytes arriving on the ESP32 UART2 RX pin |
| 2 | Valid NMEA sentence parsed (passed checksum) |
| 3 | Position fix acquired — `gps.location.isValid()` |

Once all three pass, the sketch streams a 1 Hz status line so you can verify lat/lon update as you walk around.

---

## Hardware required

- ESP32 (USB-powered for bench)
- BN-220 GPS module
- Open sky view, OR a window with the antenna pointing at it
- TinyGPSPlus library installed

> **Cold-start TTFF (time to first fix):** ~30 s outdoors with a clear sky, 60–300 s next to a window, **probably never** indoors mid-room. Don't conclude PASS 3/3 will fail until you've given the antenna real sky.

---

## Library required

Install via Arduino IDE → Library Manager:

> **TinyGPSPlus** by Mikal Hart

(If you see "TinyGPS" without the Plus, that's the older one — install TinyGPSPlus.)

---

## Wiring

| BN-220 pin | Wire color | Bench pin | Boat pin | Notes |
|-----------|------------|-----------|----------|-------|
| VCC | red | **3.3V** | 3.3V | 5V tolerated but 3.3V is safe on all batches |
| GND | black | GND | GND | |
| TX  | **white** | **GPIO 18** | **GPIO 17** | GPS TX → ESP32 RX (cross-over). Bench uses 18 because GPIO 17 was already taken by the GPS-RX line on the breadboard. |
| RX  | **green** | **GPIO 17** | **GPIO 4**  | ESP32 TX → GPS RX. Optional — leave disconnected if you don't plan to send config to the GPS. |

> **Wire colors on this batch are REVERSED from typical convention.** White = TX, green = RX, confirmed empirically 2026-05-03 after weeks of "1 byte then silence" failures under the wrong assumption. Don't trust BN-220 silkscreen or third-party docs — go by what the bench passed with. If you swap to a different batch, re-verify by checking which pin the blue LED's transitions correlate with.

---

## Flash and run

1. Open `test_10_bn220_gps/test_10_bn220_gps.ino` in Arduino IDE.
2. Board: ESP32 Dev Module. Port: your ESP32's port.
3. Upload.
4. Open Serial Monitor at **115200**.
5. Place the BN-220 antenna with sky view (outdoors or pressed against a window).

---

## Expected output (happy path)

```
========================================
  test_10_bn220_gps
========================================
UART2  baud=9600  RX=GPIO18 (← BN-220 TX)  TX=GPIO17 (→ BN-220 RX)

Cold-start TTFF (time to first fix):
  outdoors clear sky : ~30 s
  next to a window   : 60-300 s
  indoors mid-room   : probably never (RF blocked)

Step 1: waiting for any bytes from the GPS module...
----------------------------------------

PASS (1/3): GPS bytes arriving (after 250 ms).
Step 2: waiting for a valid NMEA sentence...
----------------------------------------

PASS (2/3): valid NMEA parsed (840 chars in, 4 sentences OK).
Step 3: waiting for position fix (sky view required)...
----------------------------------------
[t=    3s] chars=  2520  sentences=  12  fail=0  sats= 3  HDOP=2.10  (no fix)  UTC=14:23:45
[t=    4s] chars=  3360  sentences=  16  fail=0  sats= 5  HDOP=1.40  (no fix)  UTC=14:23:46
[t=    5s] chars=  4200  sentences=  20  fail=0  sats= 7  HDOP=1.10  (no fix)  UTC=14:23:47
...

PASS (3/3): position fix acquired (TTFF = 28 s from boot).

========================================
  ALL TESTS PASSED
========================================
Streaming 1 Hz fix data. Walk around to verify lat/lon updates.
[t=   29s] chars= 24360  sentences= 116  fail=0  sats= 8  HDOP=0.90  lat=40.123456  lon=-74.123456  alt=12.3m  UTC=14:24:11
```

---

## Failure modes the sketch surfaces

| Symptom | Meaning |
|---------|---------|
| Stuck on `Step 1`, hint says no bytes | No UART data. Check VCC, GND, and that green (GPS TX) → GPIO 18. |
| Stuck on `Step 2`, hint says wrong baud | Bytes arriving but no valid NMEA. Module is at a non-default baud. Try 4800 or 38400 in `GPS_BAUD`. |
| Sentences OK, sats stuck at 0 for >5 min | No sky view — move closer to a window, take it outside, or check the antenna isn't covered. |
| `[WARN] no GPS bytes for X ms` | Module silently reset (brownout? bad VCC?) or the RX wire fell off. |
| `fail` counter rising fast | Noisy UART line. Shorten the wire or add a small series resistor. |

---

## Pass criteria

- [x] `PASS (1/3)` — bytes arriving
- [x] `PASS (2/3)` — valid NMEA parsed
- [x] `PASS (3/3)` — position fix acquired
- [x] (visual) lat/lon update sensibly when you walk around

## Result — 2026-04-26

6 satellites acquired. **Original wiring report was wrong about colors.** Re-verified 2026-05-03: this batch has white=TX, green=RX, so the working bench wiring is **white → GPIO 18** (ESP32 RX) and **green → GPIO 17** (ESP32 TX). The 2026-04-26 PASS would have been with the wires in those physical positions but mislabeled in the notes.

In the boat, white goes to GPIO 17, green to GPIO 4 — see `test_10b` NOTES for the in-hull pass.

**Status: PASS**
