/*
 * test_10_bn220_gps.ino
 *
 * Bench test. Confirms a BN-220 GPS module is wired correctly, talking
 * NMEA, and (with sky view) acquires a position fix.
 *
 * Three pass gates:
 *   PASS 1/3  Bytes arriving on UART2 RX
 *   PASS 2/3  Valid NMEA sentence parsed (passed checksum)
 *   PASS 3/3  Position fix acquired (gps.location.isValid() == true)
 *
 * After PASS 2/3 the sketch prints a 1 Hz status line so you can watch
 * the fix come in. PASS 3/3 needs sky view — indoors mid-room you may
 * never get it. Take it outside or right next to a window.
 *
 * Wiring (BN-220, BENCH config — see test_10b for boat pins):
 *   BN-220 VCC (red)    → ESP32 3.3V         (5V usually OK but 3.3V is
 *                                              unconditionally safe)
 *   BN-220 GND (black)  → ESP32 GND
 *   BN-220 TX  (white)  → ESP32 GPIO 18  (UART2 RX in this sketch)
 *   BN-220 RX  (green)  → ESP32 GPIO 17  (UART2 TX — optional; leave
 *                                          disconnected unless you plan
 *                                          to send config to the GPS)
 *
 * Wire-color note: this batch of BN-220s has white=TX, green=RX —
 * REVERSED from the typical convention (and from what these comments
 * said before 2026-05-03). Confirmed empirically: green→17 / white→18
 * is the only configuration that produces NMEA. Don't trust silkscreen
 * or third-party docs — go by what the bench passed with.
 *
 * Library: install "TinyGPSPlus" by Mikal Hart from Library Manager.
 *
 * Default BN-220 settings: 9600 baud, 1 Hz NMEA, GPS+GLONASS.
 */

#include <TinyGPSPlus.h>

TinyGPSPlus     gps;
HardwareSerial  gpsSerial(2);

const uint8_t  GPS_RX_PIN = 18;   // ESP32 reads from this (← BN-220 TX, white)
const uint8_t  GPS_TX_PIN = 17;   // ESP32 writes to this (→ BN-220 RX, green)
const uint32_t GPS_BAUD   = 9600;

enum TestState { WAIT_BYTES, WAIT_SENTENCES, WAIT_FIX, LIVE };
TestState state = WAIT_BYTES;

unsigned long bootMs         = 0;
unsigned long firstByteMs    = 0;
unsigned long firstSentenceMs = 0;
unsigned long firstFixMs     = 0;
unsigned long lastByteMs     = 0;
unsigned long lastPrintMs    = 0;
unsigned long lastHintMs     = 0;
unsigned long lastByteWarnMs = 0;

const unsigned long PRINT_INTERVAL_MS    = 1000;   // 1 Hz status
const unsigned long HINT_INTERVAL_MS     = 10000;  // hint every 10 s while waiting
const unsigned long NO_BYTE_TIMEOUT_MS   = 5000;   // warn if bytes go silent

void printStatus() {
  unsigned long upS = (millis() - bootMs) / 1000;
  Serial.printf("[t=%5lus] chars=%6lu  sentences=%4lu  fail=%lu",
                upS,
                gps.charsProcessed(),
                gps.passedChecksum(),
                gps.failedChecksum());

  if (gps.satellites.isValid()) Serial.printf("  sats=%2d", gps.satellites.value());
  if (gps.hdop.isValid())       Serial.printf("  HDOP=%.2f", gps.hdop.hdop());

  if (gps.location.isValid()) {
    Serial.printf("  lat=%.6f  lon=%.6f", gps.location.lat(), gps.location.lng());
    if (gps.altitude.isValid()) Serial.printf("  alt=%.1fm", gps.altitude.meters());
  } else {
    Serial.print("  (no fix)");
  }

  if (gps.time.isValid()) {
    Serial.printf("  UTC=%02d:%02d:%02d",
                  gps.time.hour(), gps.time.minute(), gps.time.second());
  }
  Serial.println();
}

void printHint() {
  switch (state) {
    case WAIT_BYTES:
      Serial.printf("  ...no bytes from GPS yet (RX=GPIO%d). Check: VCC, GND, "
                    "TX→GPIO%d crossover, baud=%lu.\n",
                    GPS_RX_PIN, GPS_RX_PIN, GPS_BAUD);
      break;
    case WAIT_SENTENCES:
      Serial.printf("  ...bytes arriving (%lu chars) but no valid NMEA. "
                    "Wrong baud rate? Try 4800 or 38400.\n",
                    gps.charsProcessed());
      break;
    case WAIT_FIX:
      Serial.println("  ...searching for fix. Move outside or right next to a "
                     "window. Cold-start TTFF is ~30 s outdoors.");
      break;
    case LIVE:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  bootMs = millis();

  Serial.println("========================================");
  Serial.println("  test_10_bn220_gps");
  Serial.println("========================================");
  Serial.printf("UART2  baud=%lu  RX=GPIO%d (← BN-220 TX)  TX=GPIO%d (→ BN-220 RX)\n",
                GPS_BAUD, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println();
  Serial.println("Cold-start TTFF (time to first fix):");
  Serial.println("  outdoors clear sky : ~30 s");
  Serial.println("  next to a window   : 60-300 s");
  Serial.println("  indoors mid-room   : probably never (RF blocked)");
  Serial.println();
  Serial.println("Step 1: waiting for any bytes from the GPS module...");
  Serial.println("----------------------------------------");

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  lastHintMs  = millis();
  lastByteMs  = millis();
  lastPrintMs = millis();
}

void loop() {
  // ---- Pump bytes from the GPS through TinyGPSPlus ----
  while (gpsSerial.available()) {
    int b = gpsSerial.read();
    if (firstByteMs == 0) firstByteMs = millis();
    lastByteMs = millis();
    if (gps.encode(b) && firstSentenceMs == 0) {
      firstSentenceMs = millis();
    }
  }

  // ---- State transitions / pass gates ----
  switch (state) {
    case WAIT_BYTES:
      if (firstByteMs != 0) {
        Serial.println();
        Serial.printf("PASS (1/3): GPS bytes arriving (after %lu ms).\n",
                      firstByteMs - bootMs);
        Serial.println("Step 2: waiting for a valid NMEA sentence...");
        Serial.println("----------------------------------------");
        state = WAIT_SENTENCES;
        lastHintMs = millis();
      }
      break;

    case WAIT_SENTENCES:
      if (firstSentenceMs != 0) {
        Serial.println();
        Serial.printf("PASS (2/3): valid NMEA parsed (%lu chars in, %lu sentences OK).\n",
                      gps.charsProcessed(), gps.passedChecksum());
        Serial.println("Step 3: waiting for position fix (sky view required)...");
        Serial.println("----------------------------------------");
        state = WAIT_FIX;
        lastHintMs = millis();
      }
      break;

    case WAIT_FIX:
      if (gps.location.isValid()) {
        firstFixMs = millis();
        Serial.println();
        Serial.printf("PASS (3/3): position fix acquired (TTFF = %lu s from boot).\n",
                      (firstFixMs - bootMs) / 1000);
        Serial.println();
        Serial.println("========================================");
        Serial.println("  ALL TESTS PASSED");
        Serial.println("========================================");
        Serial.println("Streaming 1 Hz fix data. Walk around to verify lat/lon updates.");
        state = LIVE;
        lastHintMs = millis();
      }
      break;

    case LIVE:
      break;
  }

  // ---- 1 Hz status line (skipped during WAIT_BYTES — nothing to show) ----
  if (state != WAIT_BYTES && millis() - lastPrintMs >= PRINT_INTERVAL_MS) {
    printStatus();
    lastPrintMs = millis();
  }

  // ---- Hint every 10 s while still working through the gates ----
  if (state != LIVE && millis() - lastHintMs >= HINT_INTERVAL_MS) {
    printHint();
    lastHintMs = millis();
  }

  // ---- Watchdog: GPS went silent? ----
  if (firstByteMs != 0 && millis() - lastByteMs > NO_BYTE_TIMEOUT_MS &&
      millis() - lastByteWarnMs > 10000) {
    Serial.printf("[WARN] no GPS bytes for %lu ms — module reset or wire fell off?\n",
                  millis() - lastByteMs);
    lastByteWarnMs = millis();
  }
}
