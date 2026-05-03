/*
 * test_16_ibus_quality.ino
 *
 * iBUS link quality meter under live integration load.
 *
 * Drives both ESCs from the throttle stick (CH3) and the rudder from
 * CH1, while simultaneously counting good/bad iBUS frames. This is the
 * realistic noise environment: PCA9685 active on the I²C bus, three
 * channels updating at ~140 Hz, ESCs switching motor current.
 *
 * Per-window prints show current throttle alongside frame loss, so you
 * can ramp throttle by hand and watch live whether loss correlates with
 * motor current. Per-throttle-band counters give a clean histogram for
 * the run.
 *
 * Counters:
 *   good           — header OK, checksum OK
 *   bad_checksum   — header OK, checksum wrong (bit corruption mid-frame)
 *   bad_header     — 32 bytes consumed but first two weren't 0x20 0x40
 *                    (lost framing — worse failure mode)
 *   resync_bytes   — bytes discarded while hunting for the 0x20 header
 *
 * Throttle bands: 0-20%, 20-40%, 40-60%, 60-80%, 80-100% of stick
 * range (1000–2000 µs). Loss is bucketed by whichever band the throttle
 * was in when each frame arrived.
 *
 * Output every WINDOW_MS:
 *   [t= 15s] thr= 45%  win: 690 good /  10 bad (1.43%) | total: 2070/ 18 (0.86%)
 *            bands(loss%): 0-20:0.36 | 20-40:0.43 | 40-60:1.43 | 60-80: -- | 80+: --
 *
 * Commands:
 *   r + ENTER  reset all counters and band histogram
 *   d + ENTER  dump full band histogram (good/bad counts, not just %)
 *
 * SAFETY:
 *   - PROPS OFF.
 *   - Boat secured to bench / cradle. ESCs will spin both motors.
 *   - Throttle stick at zero before powering on. ESCs arm from neutral.
 *
 * Wiring:
 *   FS-iA10B iBUS SERVO → 1 kΩ → GPIO 16 → 2 kΩ → GND
 *   PCA9685 SDA/SCL → GPIO 21 / 22, V+ rail to 6 V (rudder), ESCs on V+
 *   ESCs on PCA ch0 (port) / ch1 (stbd), rudder on ch2
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
bool pcaPresent = false;

HardwareSerial ibusSerial(1);

// ---- Tunables ----
const uint32_t WINDOW_MS = 5000;   // report interval. Longer = more stable %.

// PCA9685 channels (mirror config.h CH_ESC_PORT / CH_ESC_STBD / CH_RUDDER)
const uint8_t  CH_ESC_PORT_LOCAL = 0;
const uint8_t  CH_ESC_STBD_LOCAL = 1;
const uint8_t  CH_RUDDER_LOCAL   = 2;

// iBUS channel indices (Mode 2 / FS-i6X with right-stick rudder)
const uint8_t  IDX_RUDDER   = 0;   // CH1 right-stick horizontal
const uint8_t  IDX_THROTTLE = 2;   // CH3 left-stick vertical

// Rudder clamp (matches config.h RUDDER_MIN_US / RUDDER_MAX_US)
const uint16_t RUDDER_MIN_US_LOCAL = 1330;
const uint16_t RUDDER_MAX_US_LOCAL = 1670;
const bool     RUDDER_REVERSE      = false;

// ---- iBUS framing state ----
uint8_t  ibusBuffer[32];
uint8_t  ibusIdx = 0;
uint16_t channels[10];

// ---- Counters ----
uint32_t goodTotal       = 0;
uint32_t badChecksumTotal = 0;
uint32_t badHeaderTotal   = 0;
uint32_t resyncBytesTotal = 0;

uint32_t goodWin       = 0;
uint32_t badChecksumWin = 0;
uint32_t badHeaderWin   = 0;

// Per-throttle-band counters (5 bands of 20% each)
const uint8_t NUM_BANDS = 5;
uint32_t bandGood[NUM_BANDS] = {0};
uint32_t bandBad[NUM_BANDS]  = {0};

unsigned long startMs    = 0;
unsigned long lastReport = 0;
unsigned long lastFrame  = 0;
bool gotFirstFrame = false;
bool signalLostWarned = false;
const unsigned long NO_FRAME_TIMEOUT_MS = 1000;

// Latest throttle/rudder stick values for bucketing + driving outputs
uint16_t lastThrottleUs = 1500;  // pre-arm; gets replaced by real stick value
uint16_t lastRudderUs   = 1500;

uint16_t usTicks(uint16_t microseconds) {
  return (uint16_t)((microseconds / 20000.0f) * 4096);
}

bool verifyChecksum() {
  uint16_t sum = 0xFFFF;
  for (int i = 0; i < 30; i++) sum -= ibusBuffer[i];
  uint16_t rx = ibusBuffer[30] | (ibusBuffer[31] << 8);
  return sum == rx;
}

uint16_t clampUs(uint16_t us, uint16_t lo, uint16_t hi) {
  if (us < lo) return lo;
  if (us > hi) return hi;
  return us;
}

uint8_t throttleBand(uint16_t us) {
  // Map 1000–2000 µs → band 0..4
  if (us < 1000) us = 1000;
  if (us > 2000) us = 2000;
  uint16_t pct = (us - 1000) / 10;          // 0..100
  uint8_t  band = pct / 20;                 // 0..5
  if (band >= NUM_BANDS) band = NUM_BANDS - 1;
  return band;
}

void writeNeutral() {
  if (!pcaPresent) return;
  pca.setPWM(CH_ESC_PORT_LOCAL, 0, usTicks(1500));
  pca.setPWM(CH_ESC_STBD_LOCAL, 0, usTicks(1500));
  pca.setPWM(CH_RUDDER_LOCAL,   0, usTicks(1500));
}

void resetCounters() {
  goodTotal = badChecksumTotal = badHeaderTotal = resyncBytesTotal = 0;
  goodWin = badChecksumWin = badHeaderWin = 0;
  for (uint8_t i = 0; i < NUM_BANDS; i++) {
    bandGood[i] = 0;
    bandBad[i]  = 0;
  }
  startMs = millis();
  lastReport = millis();
  Serial.println();
  Serial.println("[RESET] counters and band histogram cleared.");
  Serial.println();
}

void dumpHistogram() {
  Serial.println();
  Serial.println("---- Throttle band histogram ----");
  const char* labels[NUM_BANDS] = {" 0-20%", "20-40%", "40-60%", "60-80%", "80-100%"};
  for (uint8_t i = 0; i < NUM_BANDS; i++) {
    uint32_t tot = bandGood[i] + bandBad[i];
    if (tot == 0) {
      Serial.printf("  %s   no samples\n", labels[i]);
    } else {
      float pct = 100.0f * bandBad[i] / tot;
      Serial.printf("  %s   %5lu good / %4lu bad  (%.2f%%)\n",
                    labels[i],
                    (unsigned long)bandGood[i],
                    (unsigned long)bandBad[i],
                    pct);
    }
  }
  Serial.println("---------------------------------");
  Serial.println();
}

void printReport() {
  uint32_t winTotalFrames = goodWin + badChecksumWin + badHeaderWin;
  uint32_t winBad         = badChecksumWin + badHeaderWin;
  float    winLossPct     = winTotalFrames ? (100.0f * winBad / winTotalFrames) : 0.0f;

  uint32_t totTotalFrames = goodTotal + badChecksumTotal + badHeaderTotal;
  uint32_t totBad         = badChecksumTotal + badHeaderTotal;
  float    totLossPct     = totTotalFrames ? (100.0f * totBad / totTotalFrames) : 0.0f;

  unsigned long elapsedSec = (millis() - startMs) / 1000;
  uint16_t thrUs = lastThrottleUs;
  int      thrPct = ((int)thrUs - 1000) / 10;
  if (thrPct < 0) thrPct = 0;
  if (thrPct > 100) thrPct = 100;

  Serial.printf("[t=%3lus] thr=%3d%%  win: %4lu good / %3lu bad (%.2f%%) | total: %5lu/%4lu (%.2f%%)\n",
                elapsedSec, thrPct,
                (unsigned long)goodWin, (unsigned long)winBad, winLossPct,
                (unsigned long)goodTotal, (unsigned long)totBad, totLossPct);

  // One-line band loss summary
  Serial.print("         bands(loss%): ");
  const char* short_labels[NUM_BANDS] = {"0-20", "20-40", "40-60", "60-80", "80+"};
  for (uint8_t i = 0; i < NUM_BANDS; i++) {
    uint32_t tot = bandGood[i] + bandBad[i];
    if (tot == 0) {
      Serial.printf("%s: --", short_labels[i]);
    } else {
      Serial.printf("%s:%.2f", short_labels[i], 100.0f * bandBad[i] / tot);
    }
    if (i < NUM_BANDS - 1) Serial.print(" | ");
  }
  Serial.println();

  goodWin = badChecksumWin = badHeaderWin = 0;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_16_ibus_quality (under load)");
  Serial.println("========================================");
  Serial.println("Counts iBUS frames while driving ESCs + rudder live.");
  Serial.printf("Window: %lu ms.  Commands: 'r'=reset, 'd'=dump bands.\n",
                (unsigned long)WINDOW_MS);
  Serial.println();
  Serial.println("** SAFETY: PROPS OFF.  Boat secured.  Throttle at zero. **");
  Serial.println();
  Serial.println("Healthy: total loss <0.05%, bands flat across throttle.");
  Serial.println("Watch for: bands showing rising loss with rising throttle —");
  Serial.println("that's motor-current EMI coupling onto the iBUS line.");
  Serial.println();

  Wire.begin(21, 22);
  Wire.beginTransmission(0x40);
  if (Wire.endTransmission() == 0) {
    pcaPresent = true;
    pca.begin();
    pca.setOscillatorFrequency(27000000);
    pca.setPWMFreq(50);
    Serial.println("[INFO] PCA9685 found. Arming ESCs at 1500 µs (3 s)...");
    writeNeutral();
    delay(3000);
    Serial.println("[INFO] ESCs armed. Outputs live once iBUS acquires.");
  } else {
    Serial.println("[FAIL] PCA9685 not found at 0x40. Check I²C wiring.");
    while (true) delay(1000);
  }

  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);

  Serial.println();
  Serial.println("Waiting for iBUS frames... turn on the transmitter.");
  Serial.println("----------------------------------------");
  startMs    = millis();
  lastReport = millis();
}

void applyOutputs() {
  uint16_t thrRaw = channels[IDX_THROTTLE];
  uint16_t rudRaw = channels[IDX_RUDDER];
  if (thrRaw < 1000) thrRaw = 1000;
  if (thrRaw > 2000) thrRaw = 2000;
  if (rudRaw < 1000) rudRaw = 1000;
  if (rudRaw > 2000) rudRaw = 2000;
  if (RUDDER_REVERSE) rudRaw = 3000 - rudRaw;
  uint16_t rudUs = clampUs(rudRaw, RUDDER_MIN_US_LOCAL, RUDDER_MAX_US_LOCAL);

  lastThrottleUs = thrRaw;
  lastRudderUs   = rudUs;

  if (pcaPresent) {
    pca.setPWM(CH_ESC_PORT_LOCAL, 0, usTicks(thrRaw));
    pca.setPWM(CH_ESC_STBD_LOCAL, 0, usTicks(thrRaw));
    pca.setPWM(CH_RUDDER_LOCAL,   0, usTicks(rudUs));
  }
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'r' || c == 'R') resetCounters();
    else if (c == 'd' || c == 'D') dumpHistogram();
  }

  while (ibusSerial.available()) {
    uint8_t b = ibusSerial.read();

    if (ibusIdx == 0 && b != 0x20) {
      resyncBytesTotal++;
      continue;
    }

    ibusBuffer[ibusIdx++] = b;
    if (ibusIdx == 32) {
      // Categorize this frame.
      bool good = false;
      if (ibusBuffer[0] == 0x20 && ibusBuffer[1] == 0x40) {
        if (verifyChecksum()) {
          good = true;
          // Decode channels for output driving.
          for (int i = 0; i < 10; i++)
            channels[i] = ibusBuffer[2 + i*2] | (ibusBuffer[3 + i*2] << 8);
        } else {
          badChecksumTotal++;
          badChecksumWin++;
        }
      } else {
        badHeaderTotal++;
        badHeaderWin++;
      }

      // Bucket this frame into the band of the *current* throttle
      // (use lastThrottleUs — gets refreshed below if frame was good).
      uint8_t band = throttleBand(lastThrottleUs);
      if (good) {
        goodTotal++;
        goodWin++;
        bandGood[band]++;
      } else {
        bandBad[band]++;
      }

      if (good) {
        if (!gotFirstFrame) {
          gotFirstFrame = true;
          Serial.println("[OK] iBUS acquired. Outputs live. Counting...");
          Serial.println("----------------------------------------");
          startMs    = millis();
          lastReport = millis();
        }
        lastFrame = millis();
        applyOutputs();
        signalLostWarned = false;
      }

      ibusIdx = 0;
    }
  }

  // Failsafe: if frames stop, neutralize all outputs.
  if (gotFirstFrame && lastFrame > 0 &&
      millis() - lastFrame > NO_FRAME_TIMEOUT_MS && !signalLostWarned) {
    if (pcaPresent) writeNeutral();
    Serial.println("[WARN] iBUS signal lost — ESCs/rudder neutralized.");
    signalLostWarned = true;
  }

  if (gotFirstFrame && millis() - lastReport >= WINDOW_MS) {
    printReport();
    lastReport = millis();
  }

  if (!gotFirstFrame && millis() - lastReport >= 5000) {
    Serial.println("  ...still waiting. Check transmitter, GPIO16, divider, RX power.");
    lastReport = millis();
  }
}
