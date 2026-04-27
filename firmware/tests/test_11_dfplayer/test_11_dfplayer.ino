/*
 * test_11_dfplayer.ino
 *
 * Bench test. Confirms a DFRobot DF1201S "DFPlayer Pro" (SKU DFR0768)
 * is wired correctly, talks AT-protocol UART at 115200 baud, and plays
 * file #1 from internal flash / SD — once on startup and again every
 * 20 seconds thereafter.
 *
 * NOTE: the DF1201S is *not* the same module as the DFPlayer Mini.
 *   - DFPlayer Mini : 9600 baud, binary frames (0x7E ... 0xEF),
 *                     library "DFRobotDFPlayerMini"
 *   - DF1201S       : 115200 baud, AT command protocol,
 *                     library "DFRobot_DF1201S"
 * If you point the wrong driver at the wrong module nothing works and
 * the diagnostics are nonsense — confirmed the hard way.
 *
 * Audio path on the boat: DF1201S → mini stereo amp → 2 speakers.
 * The DF1201S has an on-board 8Ω/3W amp too (controlled by enableAMP /
 * disableAMP); whether you use it or feed line-out to the external amp
 * is up to your wiring.
 *
 * Two pass gates:
 *   PASS 1/2  DF1201S.begin() returns true (module ACKs over UART)
 *   PASS 2/2  File 1 plays — auditory verification (you hear it)
 *
 * Wiring (DF1201S, BENCH config):
 *   DF1201S VIN  → ESP32 3.3V  (5V works only with a level shifter on the
 *                                ESP32-TX → DF1201S-RX line; without one,
 *                                begin() times out. 3.3V is the safe default
 *                                used by the working Fitz reference.)
 *   DF1201S GND  → ESP32 GND   (and tie amp GND to the same point)
 *   DF1201S RX   → ESP32 GPIO 26 (this is ESP32's TX line)
 *   DF1201S TX   → ESP32 GPIO 25 (this is ESP32's RX line)
 *   DF1201S SPK+ → external amp L+, OR direct to one speaker terminal
 *   DF1201S SPK- → external amp L-, OR direct to other speaker terminal
 *
 * Sanity-check the data lines:
 *   - The pin labeled RX on the DF1201S board is an INPUT. It must
 *     connect to ESP32 GPIO 26 (the OUTPUT in this sketch).
 *   - The pin labeled TX on the DF1201S board is an OUTPUT. It must
 *     connect to ESP32 GPIO 25 (the INPUT in this sketch).
 *   - If you wire RX→RX or TX→TX, begin() will fail forever.
 *
 * Library: install "DFRobot_DF1201S" by DFRobot from Library Manager.
 *
 * The on-board flash ships with a sample track at index 1, so this test
 * works even with no SD card inserted.
 */

#include <DFRobot_DF1201S.h>

const uint8_t  DFP_RX_PIN = 25;   // ESP32 reads from this (← DF1201S TX)
const uint8_t  DFP_TX_PIN = 26;   // ESP32 writes to this (→ DF1201S RX)
const uint32_t DFP_BAUD   = 115200;
const uint8_t  DFP_VOLUME = 20;   // 0–30
const int16_t  TEST_TRACK = 1;
const unsigned long REPLAY_INTERVAL_MS = 20000;

HardwareSerial      dfSerial(2);
DFRobot_DF1201S     DF1201S;

unsigned long bootMs     = 0;
unsigned long lastPlayMs = 0;
unsigned long playCount  = 0;
bool          beginOk    = false;

void setup() {
  Serial.begin(115200);
  delay(500);
  bootMs = millis();

  Serial.println("========================================");
  Serial.println("  test_11_dfplayer (DF1201S / DFPlayer Pro)");
  Serial.println("========================================");
  Serial.printf("HardwareSerial(2)  baud=%lu  RX=GPIO%d (← DF1201S TX)  TX=GPIO%d (→ DF1201S RX)\n",
                DFP_BAUD, DFP_RX_PIN, DFP_TX_PIN);
  Serial.printf("Volume=%u/30  test track=%d  replay every %lus\n",
                DFP_VOLUME, TEST_TRACK, REPLAY_INTERVAL_MS / 1000);
  Serial.println();
  Serial.println("Step 1: opening UART and calling DF1201S.begin()...");
  Serial.println("(begin() can take 1-3 s)");
  Serial.println("----------------------------------------");

  dfSerial.begin(DFP_BAUD, SERIAL_8N1, DFP_RX_PIN, DFP_TX_PIN);
  delay(1000);   // Fitz reference uses 1 s here; shorter values fail begin() intermittently.

  // begin() returns false until the module ACKs an AT query.
  unsigned long beginStart = millis();
  while (!DF1201S.begin(dfSerial)) {
    if (millis() - beginStart > 5000) {
      Serial.println("FAIL (1/2): DF1201S did not respond to begin() within 5 s.");
      Serial.println("  Check, in this order:");
      Serial.println("    1. VCC = 3.3-5V at the DF1201S VCC pad (multimeter)");
      Serial.println("    2. GND tied between ESP32, DF1201S, and amp");
      Serial.printf( "    3. ESP32 TX (GPIO%d) → DF1201S RX\n", DFP_TX_PIN);
      Serial.printf( "    4. ESP32 RX (GPIO%d) ← DF1201S TX\n", DFP_RX_PIN);
      Serial.println("    5. Library is DFRobot_DF1201S (NOT DFRobotDFPlayerMini)");
      Serial.println("    6. Baud is 115200 (DF1201S default — NOT 9600 like the Mini)");
      Serial.println("  The sketch will sit here. Fix wiring/library and reset.");
      return;
    }
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.printf("PASS (1/2): DF1201S responded (after %lu ms).\n",
                millis() - bootMs);

  // Configure: music mode, single-shot playback, set volume.
  DF1201S.setVol(DFP_VOLUME);
  DF1201S.switchFunction(DF1201S.MUSIC);
  delay(2000);                          // SD/flash scan after switchFunction
  DF1201S.setPlayMode(DF1201S.SINGLE);  // play once and stop, so we control replay timing
  DF1201S.enableAMP();                  // safe even if you're feeding the external amp from line-out

  Serial.printf("Step 2: playing file #%d — listen for audio out of the speakers.\n",
                TEST_TRACK);
  Serial.println("----------------------------------------");
  DF1201S.playFileNum(TEST_TRACK);
  lastPlayMs = millis();
  playCount  = 1;
  beginOk    = true;
  Serial.printf("[t=%5lus]  play #%lu  file=%d\n",
                (millis() - bootMs) / 1000, playCount, TEST_TRACK);
  Serial.println();
  Serial.println("PASS (2/2) is auditory: confirm you hear file 1 from the speakers.");
  Serial.println("If silent, check: amp powered, volume, on-board AMP enabled (it is).");
}

void loop() {
  if (!beginOk) {
    delay(1000);
    return;
  }

  if (millis() - lastPlayMs >= REPLAY_INTERVAL_MS) {
    playCount++;
    Serial.printf("[t=%5lus]  play #%lu  file=%d\n",
                  (millis() - bootMs) / 1000, playCount, TEST_TRACK);
    DF1201S.playFileNum(TEST_TRACK);
    lastPlayMs = millis();
  }
}
