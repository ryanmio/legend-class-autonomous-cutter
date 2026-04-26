/*
 * test_10b_bn220_gps_boat.ino
 *
 * Same test as test_10_bn220_gps, but pinned for the BOAT (in-hull) wiring:
 *   GPS TX (green) → ESP32 GPIO 4   (UART2 RX in this sketch)
 *   GPS RX (white) → ESP32 GPIO 17  (UART2 TX, optional)
 *
 * GPIO 4 is the permanent GPS RX pin per the project pinout. test_10
 * (bench) uses GPIO 18 because that's what was wired on the breadboard.
 *
 * Three pass gates:
 *   PASS 1/3  Bytes arriving on UART2 RX
 *   PASS 2/3  Valid NMEA sentence parsed (passed checksum)
 *   PASS 3/3  Position fix acquired (gps.location.isValid() == true)
 *
 * If you don't see PASS 1/3 within a few seconds and the BN-220's LED is
 * also dark, the most likely cause is no power at the BN-220 — multimeter
 * 3.3V at the BN-220's own VCC pad before chasing pin/baud issues. The
 * GPS LED has nothing to do with which ESP pin you connected; it just
 * indicates the GPS is powered and transmitting.
 *
 * Library: install "TinyGPSPlus" by Mikal Hart from Library Manager.
 *
 * Default BN-220 settings: 9600 baud, 1 Hz NMEA, GPS+GLONASS.
 */

#include <TinyGPSPlus.h>

TinyGPSPlus     gps;
HardwareSerial  gpsSerial(2);

const uint8_t  GPS_RX_PIN = 4;    // ESP32 reads from this (← BN-220 TX, green)
const uint8_t  GPS_TX_PIN = 17;   // ESP32 writes to this (→ BN-220 RX, white)
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

const unsigned long PRINT_INTERVAL_MS    = 1000;
const unsigned long HINT_INTERVAL_MS     = 10000;
const unsigned long NO_BYTE_TIMEOUT_MS   = 5000;

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
      Serial.printf("  ...no bytes from GPS yet (RX=GPIO%d). If the GPS LED is also "
                    "DARK, the module isn't powered — multimeter 3.3V at the BN-220's "
                    "own VCC pad before suspecting the data line.\n", GPS_RX_PIN);
      break;
    case WAIT_SENTENCES:
      Serial.printf("  ...bytes arriving (%lu chars) but no valid NMEA. "
                    "Wrong baud rate? Try 4800 or 38400.\n",
                    gps.charsProcessed());
      break;
    case WAIT_FIX:
      Serial.println("  ...searching for fix. The boat antenna needs sky — open the "
                     "hatch or hold the boat near a window.");
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
  Serial.println("  test_10b_bn220_gps_boat");
  Serial.println("  (boat / in-hull pin config)");
  Serial.println("========================================");
  Serial.printf("UART2  baud=%lu  RX=GPIO%d (← BN-220 TX, green)  TX=GPIO%d (→ BN-220 RX, white)\n",
                GPS_BAUD, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println();
  Serial.println("If the BN-220 LED is dark, suspect power at the module first.");
  Serial.println("Multimeter VCC at the BN-220's own pad — not upstream.");
  Serial.println();
  Serial.println("Step 1: waiting for any bytes from the GPS module...");
  Serial.println("----------------------------------------");

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  lastHintMs  = millis();
  lastByteMs  = millis();
  lastPrintMs = millis();
}

void loop() {
  while (gpsSerial.available()) {
    int b = gpsSerial.read();
    if (firstByteMs == 0) firstByteMs = millis();
    lastByteMs = millis();
    if (gps.encode(b) && firstSentenceMs == 0) {
      firstSentenceMs = millis();
    }
  }

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
        Serial.println("  ALL TESTS PASSED — BOAT GPS WIRING OK");
        Serial.println("========================================");
        Serial.println("Streaming 1 Hz fix data. Walk the boat around to verify lat/lon updates.");
        state = LIVE;
        lastHintMs = millis();
      }
      break;

    case LIVE:
      break;
  }

  if (state != WAIT_BYTES && millis() - lastPrintMs >= PRINT_INTERVAL_MS) {
    printStatus();
    lastPrintMs = millis();
  }

  if (state != LIVE && millis() - lastHintMs >= HINT_INTERVAL_MS) {
    printHint();
    lastHintMs = millis();
  }

  if (firstByteMs != 0 && millis() - lastByteMs > NO_BYTE_TIMEOUT_MS &&
      millis() - lastByteWarnMs > 10000) {
    Serial.printf("[WARN] no GPS bytes for %lu ms — module reset or wire fell off?\n",
                  millis() - lastByteMs);
    lastByteWarnMs = millis();
  }
}
