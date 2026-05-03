/*
 * test_16_ibus_quality.ino
 *
 * iBUS link quality meter. Counts good frames vs bad frames over a
 * rolling window so you can compare configurations (twist wires, add
 * filter cap, reroute, etc.) and see whether a change actually helped.
 *
 * No PCA9685, no servo — just the iBUS path. Identical to test_03's
 * wiring. Run this BEFORE and AFTER each mitigation; compare the loss %.
 *
 * Counters:
 *   good           — frame parsed cleanly (header OK, checksum OK)
 *   bad_checksum   — header OK but checksum wrong (bit corruption mid-frame)
 *   bad_header     — 32 bytes consumed but first two weren't 0x20 0x40
 *                    (lost framing — usually means a worse noise problem)
 *   resync_bytes   — bytes discarded while hunting for the 0x20 header
 *                    (some at startup is normal; ongoing growth is bad)
 *
 * Output every WINDOW_MS:
 *   [t=10s]  win: 698 good /  4 bad (0.57%) | total: 1392/  8 (0.57%)
 *            resync_bytes=0  bad_header=0
 *
 * Type 'r'+ENTER to reset all counters (use this after applying a fix).
 *
 * Wiring (same as test_03):
 *   FS-iA10B iBUS SERVO → 1 kΩ → GPIO 16 → 2 kΩ → GND
 *   Receiver V+ from BEC, GND tied to ESP32 GND.
 */

HardwareSerial ibusSerial(1);

// ---- Tunables ----
const uint32_t WINDOW_MS = 5000;   // report interval. Longer = more stable %.

// ---- iBUS framing state ----
uint8_t  ibusBuffer[32];
uint8_t  ibusIdx = 0;

// ---- Counters ----
uint32_t goodTotal       = 0;
uint32_t badChecksumTotal = 0;
uint32_t badHeaderTotal   = 0;
uint32_t resyncBytesTotal = 0;

uint32_t goodWin       = 0;
uint32_t badChecksumWin = 0;
uint32_t badHeaderWin   = 0;

unsigned long startMs    = 0;
unsigned long lastReport = 0;
unsigned long lastFrame  = 0;
bool gotFirstFrame = false;

bool verifyChecksum() {
  uint16_t sum = 0xFFFF;
  for (int i = 0; i < 30; i++) sum -= ibusBuffer[i];
  uint16_t rx = ibusBuffer[30] | (ibusBuffer[31] << 8);
  return sum == rx;
}

void resetCounters() {
  goodTotal = badChecksumTotal = badHeaderTotal = resyncBytesTotal = 0;
  goodWin = badChecksumWin = badHeaderWin = 0;
  startMs = millis();
  lastReport = millis();
  Serial.println();
  Serial.println("[RESET] counters cleared. New window starts now.");
  Serial.println();
}

void printReport() {
  uint32_t winTotalFrames   = goodWin + badChecksumWin + badHeaderWin;
  uint32_t winBad           = badChecksumWin + badHeaderWin;
  float    winLossPct       = winTotalFrames ? (100.0f * winBad / winTotalFrames) : 0.0f;

  uint32_t totTotalFrames   = goodTotal + badChecksumTotal + badHeaderTotal;
  uint32_t totBad           = badChecksumTotal + badHeaderTotal;
  float    totLossPct       = totTotalFrames ? (100.0f * totBad / totTotalFrames) : 0.0f;

  unsigned long elapsedSec  = (millis() - startMs) / 1000;

  Serial.printf("[t=%3lus]  win: %4lu good / %3lu bad (%.2f%%) | total: %5lu/%4lu (%.2f%%)\n",
                elapsedSec,
                (unsigned long)goodWin, (unsigned long)winBad, winLossPct,
                (unsigned long)goodTotal, (unsigned long)totBad, totLossPct);
  Serial.printf("          resync_bytes=%lu  bad_header=%lu\n",
                (unsigned long)resyncBytesTotal,
                (unsigned long)badHeaderTotal);

  goodWin = badChecksumWin = badHeaderWin = 0;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_16_ibus_quality");
  Serial.println("========================================");
  Serial.println("Counts iBUS frame quality over rolling windows.");
  Serial.println("No PCA9685 / servos. iBUS path only.");
  Serial.printf("Window: %lu ms.  Type 'r'+ENTER to reset counters.\n",
                (unsigned long)WINDOW_MS);
  Serial.println();
  Serial.println("Healthy: total loss <0.05%, resync_bytes flat after startup.");
  Serial.println("Marginal: 0.05–0.5%. Bad: >0.5% or resync_bytes growing.");
  Serial.println();

  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);

  Serial.println("Waiting for iBUS frames... turn on the transmitter.");
  Serial.println("----------------------------------------");
  startMs    = millis();
  lastReport = millis();
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'r' || c == 'R') resetCounters();
  }

  while (ibusSerial.available()) {
    uint8_t b = ibusSerial.read();

    // Resync hunt: discard bytes until we see a 0x20 header.
    if (ibusIdx == 0 && b != 0x20) {
      resyncBytesTotal++;
      continue;
    }

    ibusBuffer[ibusIdx++] = b;
    if (ibusIdx == 32) {
      // Frame collected. Categorize.
      if (ibusBuffer[0] == 0x20 && ibusBuffer[1] == 0x40) {
        if (verifyChecksum()) {
          goodTotal++;
          goodWin++;
          if (!gotFirstFrame) {
            gotFirstFrame = true;
            Serial.println("[OK] iBUS acquired. Counting...");
            Serial.println("----------------------------------------");
            startMs    = millis();   // reset window start so first row is clean
            lastReport = millis();
          }
          lastFrame = millis();
        } else {
          badChecksumTotal++;
          badChecksumWin++;
        }
      } else {
        badHeaderTotal++;
        badHeaderWin++;
      }
      ibusIdx = 0;
    }
  }

  if (gotFirstFrame && millis() - lastReport >= WINDOW_MS) {
    printReport();
    lastReport = millis();
  }

  // No-signal hint
  if (!gotFirstFrame && millis() - lastReport >= 5000) {
    Serial.println("  ...still waiting. Check transmitter, GPIO16 wiring, divider, RX power.");
    lastReport = millis();
  }
}
