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

| BN-220 pin | Color (typical) | ESP32 pin | Notes |
|-----------|-----------------|-----------|-------|
| VCC | red | **3.3V** | 5V is also tolerated by most BN-220 modules but 3.3V is unconditionally safe — use 3.3V on the bench. |
| GND | black | GND | |
| TX  | yellow | **GPIO 17** | UART2 RX in this sketch — the cross-over: GPS TX → ESP RX. |
| RX  | white | **GPIO 18** | UART2 TX. Optional — leave disconnected unless you plan to send config commands to the GPS. |

> Color codes vary by batch. Verify against the silkscreen on the BN-220 PCB if your colors look different.

GPIO 16 is reserved for iBUS (test_03), so this sketch remaps UART2 to GPIO 17/18 explicitly.

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
UART2  baud=9600  RX=GPIO17 (← BN-220 TX)  TX=GPIO18 (→ BN-220 RX)

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
| Stuck on `Step 1`, hint says no bytes | No UART data. Check VCC, GND, and that TX→GPIO 17 (TX/RX cross). |
| Stuck on `Step 2`, hint says wrong baud | Bytes arriving but no valid NMEA. Module is at a non-default baud. Try 4800 or 38400 in `GPS_BAUD`. |
| Sentences OK, sats stuck at 0 for >5 min | No sky view — move closer to a window, take it outside, or check the antenna isn't covered. |
| `[WARN] no GPS bytes for X ms` | Module silently reset (brownout? bad VCC?) or the RX wire fell off. |
| `fail` counter rising fast | Noisy UART line. Shorten the wire or add a small series resistor. |

---

## Pass criteria

- [ ] `PASS (1/3)` — bytes arriving
- [ ] `PASS (2/3)` — valid NMEA parsed
- [ ] `PASS (3/3)` — position fix acquired
- [ ] (visual) lat/lon update sensibly when you walk around

## Status

Pending bench test.
