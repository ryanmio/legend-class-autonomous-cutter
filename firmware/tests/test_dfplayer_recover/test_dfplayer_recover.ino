/*
 * test_dfplayer_recover.ino
 *
 * Diagnostic + recovery sketch for a DF1201S that's stuck auto-looping
 * track 1 on every power-up (the SINGLECYCLE-persisted state we hit
 * 2026-05-16). Uses the test_11 HardwareSerial(2) path so begin() will
 * reliably ACK (we already confirmed comms work, the issue is module
 * state).
 *
 * What this does:
 *   1. begin() — print initial play-mode + isPlaying state.
 *   2. pause()                          — try the obvious stop.
 *   3. setVol(0)                        — silence the DAC even if play continues.
 *   4. disableAMP()                     — silence the onboard amp.
 *   5. setPlayMode(SINGLE) + readback   — try to overwrite the bad mode.
 *   6. switchFunction(UFDISK) + back    — hard mode bounce to break stuck state.
 *   7. Idle loop printing state every 2 s.
 *
 * Interpreting the result:
 *   - Audio silences after step 3 (vol 0)        → DAC reachable. Module not damaged.
 *   - Audio silences after step 4 (amp off)      → onboard amp reachable. Probably not damaged.
 *   - Audio silences only after step 6 (UFDISK)  → playback path is stuck on resume-on-boot.
 *   - Audio NEVER silences                       → audio output is bypassing vol+amp control.
 *                                                  That's actual damage / firmware corruption.
 *
 * Wiring (same as test_11 — no rewiring needed):
 *   DF1201S VIN → 3.3V   (5V kills begin via TX-level mismatch — do NOT use 5V)
 *   DF1201S GND → ESP32 GND
 *   DF1201S TX  → ESP32 GPIO 25 (ESP RX)
 *   DF1201S RX  → ESP32 GPIO 26 (ESP TX)
 */

#include <DFRobot_DF1201S.h>

const uint8_t  DFP_RX_PIN = 25;
const uint8_t  DFP_TX_PIN = 26;
const uint32_t DFP_BAUD   = 115200;

HardwareSerial   dfSerial(2);
DFRobot_DF1201S  DF1201S;

static const char* modeName(DFRobot_DF1201S::ePlayMode_t m) {
  switch (m) {
    case DFRobot_DF1201S::SINGLECYCLE: return "SINGLECYCLE (loop one)";
    case DFRobot_DF1201S::ALLCYCLE:    return "ALLCYCLE (loop all)";
    case DFRobot_DF1201S::SINGLE:      return "SINGLE (play once)";
    case DFRobot_DF1201S::RANDOM:      return "RANDOM";
    case DFRobot_DF1201S::FOLDER:      return "FOLDER";
    default:                           return "?";
  }
}

static void dumpState(const char* label) {
  Serial.printf("  [%s] mode=%s, isPlaying=%d\n",
                label, modeName(DF1201S.getPlayMode()), (int)DF1201S.isPlaying());
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n========================================");
  Serial.println("  test_dfplayer_recover");
  Serial.println("========================================");
  Serial.println("Listen to the speaker through every step.");
  Serial.println("Each step waits 2 s after issuing — that's");
  Serial.println("your window to hear if audio stopped.\n");

  dfSerial.begin(DFP_BAUD, SERIAL_8N1, DFP_RX_PIN, DFP_TX_PIN);
  delay(1000);

  Serial.print("begin()");
  uint32_t t0 = millis();
  while (!DF1201S.begin(dfSerial)) {
    if (millis() - t0 > 5000) {
      Serial.println("\nFAIL: begin() did not respond in 5 s. Module unreachable.");
      while (true) delay(1000);
    }
    delay(250);
    Serial.print(".");
  }
  Serial.printf(" OK (%lu ms)\n\n", millis() - t0);

  dumpState("INITIAL — before any commands");

  Serial.println("\n--- Step 1: pause() ---");
  DF1201S.pause();
  delay(2000);
  dumpState("after pause");

  Serial.println("\n--- Step 2: setVol(0) ---");
  Serial.println("  (DAC volume to zero; speaker should be silent even if playback continues)");
  DF1201S.setVol(0);
  delay(2000);
  dumpState("after vol 0");

  Serial.println("\n--- Step 3: disableAMP() ---");
  Serial.println("  (kill the onboard 8R/3W amp; SPK+/- output should be silent)");
  DF1201S.disableAMP();
  delay(2000);
  dumpState("after amp off");

  Serial.println("\n--- Step 4: setPlayMode(SINGLE) ---");
  bool ok = DF1201S.setPlayMode(DFRobot_DF1201S::SINGLE);
  Serial.printf("  setPlayMode(SINGLE) returned %s\n", ok ? "true" : "false");
  delay(500);
  dumpState("after setPlayMode SINGLE");
  Serial.println("  (read it back — if mode is still SINGLECYCLE, the write didn't persist)");

  Serial.println("\n--- Step 5: switchFunction(UFDISK) ---");
  Serial.println("  (move module out of MUSIC entirely; should fully stop playback)");
  DF1201S.switchFunction(DFRobot_DF1201S::UFDISK);
  delay(3000);

  Serial.println("\n--- Step 6: switchFunction back to MUSIC, leave paused ---");
  DF1201S.switchFunction(DFRobot_DF1201S::MUSIC);
  delay(2000);
  DF1201S.setPlayMode(DFRobot_DF1201S::SINGLE);
  DF1201S.pause();
  DF1201S.setVol(0);
  DF1201S.disableAMP();
  delay(1000);
  dumpState("FINAL — should be SINGLE + not playing");

  Serial.println("\n========================================");
  Serial.println("Recovery sequence complete. Power-cycle");
  Serial.println("the boat and watch whether the module");
  Serial.println("auto-plays on boot before re-flashing test_29.");
  Serial.println("========================================\n");
}

void loop() {
  delay(2000);
  dumpState("idle");
}
