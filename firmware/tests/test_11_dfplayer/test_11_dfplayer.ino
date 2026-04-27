/*
 * test_11_dfplayer.ino
 *
 * Bench test. Confirms a DFPlayer Mini MP3 module is wired correctly,
 * boots cleanly, and plays track 0001.mp3 from the microSD card —
 * once on startup and again every 20 seconds thereafter.
 *
 * Audio path on the boat: DFPlayer Mini → mini stereo amp → 2 speakers.
 * The amp/speakers are passive on the data side, so this test only
 * exercises the UART control link and the DFPlayer's own DAC output.
 *
 * Two pass gates:
 *   PASS 1/2  dfPlayer.begin() returns true (module ACKs over UART)
 *   PASS 2/2  Track 1 plays — auditory verification (you hear it)
 *
 * After PASS 2/2, the sketch replays track 1 every 20 s and prints
 * any DFPlayer events (track-finished, errors) as they arrive.
 *
 * Wiring (DFPlayer Mini, BENCH config):
 *   DFPlayer VCC   → ESP32 5V (DFPlayer needs 5V; 3.3V will not boot it)
 *   DFPlayer GND   → ESP32 GND  (and tie amp GND to the same point)
 *   DFPlayer RX    → ESP32 GPIO 25 (ESP32 TX)  ← 1k series resistor
 *                                                 recommended (5V part,
 *                                                 3.3V driver)
 *   DFPlayer TX    → ESP32 GPIO 26 (ESP32 RX)
 *   DFPlayer SPK_1 → amp L input
 *   DFPlayer SPK_2 → amp R input  (or use DAC_R/DAC_L for line-level)
 *   DFPlayer GND   → amp GND (common ground is mandatory)
 *
 * NOTE: ESP32-side pins are RX=26, TX=25 — matches legend_cutter/config.h
 *       (DFPLAYER_RX_PIN=26, DFPLAYER_TX_PIN=25), so the boat firmware
 *       can reuse this wiring as-is.
 *
 * microSD prep:
 *   - Format FAT32.
 *   - Place file named exactly "0001.mp3" in the SD root (or in /mp3/).
 *   - DFPlayer indexes by physical file order, not by filename number,
 *     so put 0001.mp3 on the card FIRST (before copying anything else).
 *
 * Library: install "DFRobotDFPlayerMini" by DFRobot from Library Manager.
 *
 * DFPlayer Mini default: 9600 baud UART, volume 0–30.
 */

#include <DFRobotDFPlayerMini.h>

const uint8_t  DFP_RX_PIN = 26;   // ESP32 reads from this (← DFPlayer TX)
const uint8_t  DFP_TX_PIN = 25;   // ESP32 writes to this (→ DFPlayer RX)
const uint32_t DFP_BAUD   = 9600;
const uint8_t  DFP_VOLUME = 20;   // 0–30; bench test value, dial in for room
const uint8_t  TEST_TRACK = 1;
const unsigned long REPLAY_INTERVAL_MS = 20000;

// Bench test uses HardwareSerial(2) on custom pins — all three ESP32 UARTs
// are free on the bench. Main firmware uses SoftwareSerial because UART1
// (iBUS) and UART2 (GPS) are both occupied at runtime.
HardwareSerial         dfSerial(2);
DFRobotDFPlayerMini    dfPlayer;

unsigned long bootMs        = 0;
unsigned long lastPlayMs    = 0;
unsigned long playCount     = 0;
bool          beginOk       = false;
bool          blindMode     = false;   // true = strict begin() failed; sending commands without ACK

void printDFEvent(uint8_t type, int value) {
  switch (type) {
    case TimeOut:        Serial.println("  [DF] timeout (no reply)"); break;
    case WrongStack:     Serial.println("  [DF] wrong stack"); break;
    case DFPlayerCardInserted: Serial.println("  [DF] SD card inserted"); break;
    case DFPlayerCardRemoved:  Serial.println("  [DF] SD card removed"); break;
    case DFPlayerCardOnline:   Serial.println("  [DF] SD card online"); break;
    case DFPlayerUSBInserted:  Serial.println("  [DF] USB inserted"); break;
    case DFPlayerUSBRemoved:   Serial.println("  [DF] USB removed"); break;
    case DFPlayerPlayFinished:
      Serial.printf("  [DF] track %d finished\n", value); break;
    case DFPlayerError:
      Serial.printf("  [DF] error code %d ", value);
      switch (value) {
        case Busy:           Serial.println("(busy / card not found)"); break;
        case Sleeping:       Serial.println("(sleeping)"); break;
        case SerialWrongStack: Serial.println("(serial wrong stack)"); break;
        case CheckSumNotMatch: Serial.println("(checksum mismatch)"); break;
        case FileIndexOut:   Serial.println("(file index out of range — track missing on SD)"); break;
        case FileMismatch:   Serial.println("(file mismatch)"); break;
        case Advertise:      Serial.println("(in advertise)"); break;
        default:             Serial.println("(unknown)"); break;
      }
      break;
    default:
      Serial.printf("  [DF] type=%u value=%d\n", type, value);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  bootMs = millis();

  Serial.println("========================================");
  Serial.println("  test_11_dfplayer");
  Serial.println("========================================");
  Serial.printf("HardwareSerial(2)  baud=%lu  RX=GPIO%d (← DFP TX)  TX=GPIO%d (→ DFP RX)\n",
                DFP_BAUD, DFP_RX_PIN, DFP_TX_PIN);
  Serial.printf("Volume=%u/30  test track=%u  replay every %lus\n",
                DFP_VOLUME, TEST_TRACK, REPLAY_INTERVAL_MS / 1000);
  Serial.println();
  Serial.println("Step 1: opening UART and calling dfPlayer.begin()...");
  Serial.println("(begin() can take ~3 s on a cold module while the SD spins up)");
  Serial.println("----------------------------------------");

  dfSerial.begin(DFP_BAUD, SERIAL_8N1, DFP_RX_PIN, DFP_TX_PIN);
  delay(200);

  if (dfPlayer.begin(dfSerial)) {
    beginOk = true;
    Serial.printf("PASS (1/2): DFPlayer responded (after %lu ms).\n",
                  millis() - bootMs);
  } else {
    // Strict begin() (which expects an ACK frame back from the DFPlayer)
    // failed. Fall back to BLIND mode: skip the ACK handshake entirely
    // and just send commands. This isolates the failure:
    //   - If audio plays in blind mode → forward path (ESP32→DFPlayer)
    //     works; the return path (DFPlayer→ESP32) is what's broken
    //     (bad RX wire, GND issue, or DFPlayer firmware quirk).
    //   - If still silent in blind mode → forward path is also broken
    //     (TX wire, GND, power), and no library setting will help.
    Serial.println("WARN: strict begin() failed — DFPlayer did not ACK.");
    Serial.println("  Falling back to BLIND MODE: sending commands without waiting for ACK.");
    Serial.println("  Watch for raw bytes printed below — those are what (if anything) the");
    Serial.println("  DFPlayer is sending back. If audio plays anyway, the return path is");
    Serial.println("  the only broken link. If audio is still silent, check GND continuity");
    Serial.printf( "  and TX wiring (GPIO%d → DFPlayer RX) before anything else.\n",
                   DFP_TX_PIN);
    Serial.println("----------------------------------------");
    dfPlayer.begin(dfSerial, /*isACK=*/false, /*doReset=*/false);
    blindMode = true;
    beginOk   = true;
  }

  dfPlayer.volume(DFP_VOLUME);
  delay(100);
  Serial.printf("Step 2: playing track %u — listen for audio out of the speakers.\n",
                TEST_TRACK);
  Serial.println("----------------------------------------");
  dfPlayer.play(TEST_TRACK);
  lastPlayMs = millis();
  playCount  = 1;
  Serial.printf("[t=%5lus]  play #%lu  track=%u%s\n",
                (millis() - bootMs) / 1000, playCount, TEST_TRACK,
                blindMode ? "  (blind mode)" : "");
  Serial.println();
  if (blindMode) {
    Serial.println("Listening for raw bytes from the DFPlayer's TX line. Each line below");
    Serial.println("starting with [raw] is one byte the ESP32 RX pin received. Expected on");
    Serial.println("a healthy module: a 10-byte frame starting 0x7E ... 0xEF.");
  } else {
    Serial.println("PASS (2/2) is auditory: confirm you hear track 1 from the speakers.");
    Serial.println("If silent, check: SD inserted, 0001.mp3 in root, amp powered, volume.");
  }
}

void loop() {
  if (!beginOk) {
    delay(1000);
    return;
  }

  if (blindMode) {
    // ---- Blind mode: dump raw bytes coming back on dfSerial ----
    // The library is configured with isACK=false, so it will not consume
    // these bytes. We see whatever the DFPlayer is actually transmitting.
    while (dfSerial.available()) {
      uint8_t b = dfSerial.read();
      Serial.printf("  [raw] 0x%02X\n", b);
    }
  } else {
    // ---- Strict mode: drain library event queue ----
    if (dfPlayer.available()) {
      printDFEvent(dfPlayer.readType(), dfPlayer.read());
    }
  }

  // ---- Replay track 1 every 20 s ----
  if (millis() - lastPlayMs >= REPLAY_INTERVAL_MS) {
    playCount++;
    Serial.printf("[t=%5lus]  play #%lu  track=%u%s\n",
                  (millis() - bootMs) / 1000, playCount, TEST_TRACK,
                  blindMode ? "  (blind mode)" : "");
    dfPlayer.play(TEST_TRACK);
    lastPlayMs = millis();
  }
}
