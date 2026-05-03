/*
 * test_18_deck_gun_pan.ino
 *
 * Phase 2 (active): CH5 knob → PCA9685 ch8 deck gun pan servo.
 *   * Pass-through: knob µs → servo µs, clamped to [PAN_MIN_US..PAN_MAX_US].
 *   * Default clamp 1200..1800 µs (±300 from 1500 neutral) — conservative;
 *     widen via the constants below once you've watched the linkage sweep.
 *   * No-frame and channel-freeze failsafes hold pan at 1500 µs on iBUS loss.
 *   * Running min/max of commanded pan µs printed every 2 s — same idea as
 *     test_15, so you can dial in tighter mechanical limits after observing
 *     a full sweep.
 *
 * Phase 1 (channel discovery, completed 2026-05-03 → CH5 / idx 4):
 *   The discovery sketch is preserved in the #if 0 block at the bottom of
 *   this file. Re-enable it (set to #if 1) if you ever need to rediscover
 *   a knob's channel on a different transmitter mapping.
 *
 * NOTE: config.h:63 says CH_GUN_PAN 3 — that's the pre-hardware scaffold
 * value. Real wiring is PCA9685 ch8 (this test). Update config.h once
 * phase 2 passes.
 *
 * Wiring (phase 2):
 *   Receiver iBUS    → 1 kΩ → GPIO 16 (with 2 kΩ to GND)
 *   Pan servo signal → PCA9685 ch8
 *   Pan servo V+/GND → PCA9685 V+ rail / GND  (servo voltage to V+)
 *   PCA9685 SDA/SCL  → GPIO 21 / 22
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
bool pcaPresent = false;

HardwareSerial ibusSerial(1);

// ---- Tunables ----
const uint8_t  KNOB_CHANNEL_INDEX   = 4;     // CH5, found in phase 1
const uint8_t  GUN_PAN_PCA_CHANNEL  = 8;     // PCA9685 ch8 (real wiring)
const uint16_t PAN_MIN_US           = 1200;  // start conservative; tighten if binding
const uint16_t PAN_MAX_US           = 1800;
const bool     PAN_REVERSE          = false; // flip if knob CW = pan left

const uint16_t REPORT_INTERVAL_MS   = 2000;

// ---- iBUS state ----
uint8_t  ibusBuffer[32];
uint8_t  ibusIdx = 0;
uint16_t channels[10];
uint16_t prevChannels[10];
bool     prevValid = false;
unsigned long lastFrame         = 0;
unsigned long lastChannelChange = 0;
unsigned long lastWarn          = 0;

// ---- Failsafe ----
bool failsafeActive = false;
const unsigned long FREEZE_TIMEOUT_MS   = 500;
const unsigned long NO_FRAME_TIMEOUT_MS = 500;

// ---- Running observed pan range ----
uint16_t minPanCmdUs = 1500;
uint16_t maxPanCmdUs = 1500;
bool     gotSignal   = false;
unsigned long lastReport = 0;

uint16_t usTicks(uint16_t microseconds) {
  return (uint16_t)((microseconds / 20000.0f) * 4096);
}

uint16_t clampPan(uint16_t rawUs) {
  if (rawUs < 1000) rawUs = 1000;
  if (rawUs > 2000) rawUs = 2000;
  if (PAN_REVERSE) rawUs = 3000 - rawUs;
  if (rawUs < PAN_MIN_US) rawUs = PAN_MIN_US;
  if (rawUs > PAN_MAX_US) rawUs = PAN_MAX_US;
  return rawUs;
}

void writePan(uint16_t panUs) {
  if (!pcaPresent) return;
  pca.setPWM(GUN_PAN_PCA_CHANNEL, 0, usTicks(panUs));
}

void writeNeutral() { writePan(1500); }

bool parseIBus() {
  if (ibusBuffer[0] != 0x20 || ibusBuffer[1] != 0x40) return false;
  uint16_t sum = 0xFFFF;
  for (int i = 0; i < 30; i++) sum -= ibusBuffer[i];
  uint16_t rx = ibusBuffer[30] | (ibusBuffer[31] << 8);
  if (sum != rx) {
    if (millis() - lastWarn > 1000) {
      Serial.println("[WARN] checksum mismatch — wiring noise?");
      lastWarn = millis();
    }
    return false;
  }
  for (int i = 0; i < 10; i++)
    channels[i] = ibusBuffer[2 + i*2] | (ibusBuffer[3 + i*2] << 8);
  lastFrame = millis();
  return true;
}

void enterFailsafe(const char* reason) {
  if (failsafeActive) return;
  failsafeActive = true;
  writeNeutral();
  Serial.println();
  Serial.printf(">>> FAILSAFE — %s. Pan held at 1500. <<<\n", reason);
}

void clearFailsafe() {
  if (!failsafeActive) return;
  failsafeActive = false;
  Serial.println();
  Serial.println(">>> FAILSAFE RECOVERED — pan tracking knob again. <<<");
}

void handleFrame() {
  // ---- Channel-freeze detection ----
  bool anyDiff = false;
  if (prevValid) {
    for (int i = 0; i < 10; i++) {
      if (channels[i] != prevChannels[i]) { anyDiff = true; break; }
    }
  }
  if (anyDiff) {
    lastChannelChange = millis();
    if (failsafeActive) clearFailsafe();
  }
  for (int i = 0; i < 10; i++) prevChannels[i] = channels[i];
  prevValid = true;

  if (!failsafeActive &&
      millis() - lastChannelChange > FREEZE_TIMEOUT_MS) {
    enterFailsafe("channels frozen (TX off?)");
  }

  uint16_t knobRaw = channels[KNOB_CHANNEL_INDEX];
  uint16_t panUs   = failsafeActive ? 1500 : clampPan(knobRaw);
  writePan(panUs);

  if (!gotSignal) {
    gotSignal = true;
    Serial.println();
    Serial.printf("[OK] iBUS acquired. CH%d=%u µs → pan=%u µs.\n",
                  KNOB_CHANNEL_INDEX + 1, knobRaw, panUs);
    Serial.println("Turn the knob — pan servo should track. Reports every 2 s.");
    Serial.println("----------------------------------------");
  }

  if (panUs < minPanCmdUs) minPanCmdUs = panUs;
  if (panUs > maxPanCmdUs) maxPanCmdUs = panUs;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_18_deck_gun_pan — phase 2: knob → ch8 servo");
  Serial.println("========================================");
  Serial.printf("Knob:    CH%d (idx %d)\n",
                KNOB_CHANNEL_INDEX + 1, KNOB_CHANNEL_INDEX);
  Serial.printf("Pan servo: PCA9685 ch%d\n", GUN_PAN_PCA_CHANNEL);
  Serial.printf("Clamp:   %u..%u µs   reverse=%s\n",
                PAN_MIN_US, PAN_MAX_US, PAN_REVERSE ? "true" : "false");
  Serial.println();

  Wire.begin(21, 22);
  Wire.beginTransmission(0x40);
  if (Wire.endTransmission() == 0) {
    pcaPresent = true;
    pca.begin();
    pca.setOscillatorFrequency(27000000);
    pca.setPWMFreq(50);
    Serial.println("[INFO] PCA9685 found — pan centered at 1500 µs.");
    writeNeutral();
  } else {
    Serial.println("[FAIL] PCA9685 not found at 0x40. Check I2C wiring (SDA=21, SCL=22).");
    while (true) delay(1000);
  }

  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);

  Serial.println();
  Serial.println("Waiting for iBUS frames... turn on the transmitter.");
  Serial.println("----------------------------------------");
  lastReport = millis();
}

void loop() {
  while (ibusSerial.available()) {
    uint8_t b = ibusSerial.read();
    if (ibusIdx == 0 && b != 0x20) continue;
    ibusBuffer[ibusIdx++] = b;
    if (ibusIdx == 32) {
      if (parseIBus()) handleFrame();
      ibusIdx = 0;
    }
  }

  // No-frame failsafe.
  if (gotSignal && !failsafeActive &&
      millis() - lastFrame > NO_FRAME_TIMEOUT_MS) {
    enterFailsafe("no iBUS frames (RX disconnected?)");
  }

  // Periodic report — pan command range so far.
  if (gotSignal && millis() - lastReport >= REPORT_INTERVAL_MS) {
    Serial.printf("Pan range so far: %u..%u µs   (Δ from 1500: %+d / %+d)\n",
                  minPanCmdUs, maxPanCmdUs,
                  (int)minPanCmdUs - 1500, (int)maxPanCmdUs - 1500);
    lastReport = millis();
  }
}


// ============================================================================
// PHASE 1 — channel discovery sketch, kept for reference.
// Re-enable by changing #if 0 to #if 1 (and disabling phase 2 above) if you
// need to rediscover a knob's channel for a different TX mapping.
// ============================================================================
#if 0

HardwareSerial ibusSerial(1);

uint8_t  ibusBuffer[32];
uint8_t  ibusIdx = 0;
uint16_t channels[10];
uint16_t prevChannels[10];
bool     prevValid = false;

unsigned long lastFrame  = 0;
unsigned long lastWarn   = 0;
unsigned long framesSeen = 0;
unsigned long lastChanPrint[10] = {0};

const uint16_t      MOVE_THRESHOLD_US     = 20;
const unsigned long PER_CHANNEL_RATE_MS   = 150;
const unsigned long NO_FRAME_TIMEOUT_MS   = 1000;
const unsigned long FIRST_FRAME_HINT_MS   = 5000;
unsigned long lastHint = 0;

bool parseIBus() {
  if (ibusBuffer[0] != 0x20 || ibusBuffer[1] != 0x40) return false;
  uint16_t sum = 0xFFFF;
  for (int i = 0; i < 30; i++) sum -= ibusBuffer[i];
  uint16_t rx = ibusBuffer[30] | (ibusBuffer[31] << 8);
  if (sum != rx) { return false; }
  for (int i = 0; i < 10; i++)
    channels[i] = ibusBuffer[2 + i*2] | (ibusBuffer[3 + i*2] << 8);
  lastFrame = millis();
  framesSeen++;
  return true;
}

void handleFrame() {
  if (!prevValid) {
    for (int i = 0; i < 10; i++) prevChannels[i] = channels[i];
    prevValid = true;
    Serial.print("[OK] initial values: ");
    for (int i = 0; i < 10; i++) Serial.printf("CH%d=%u ", i + 1, channels[i]);
    Serial.println();
    return;
  }
  unsigned long now = millis();
  for (int i = 0; i < 10; i++) {
    int delta = (int)channels[i] - (int)prevChannels[i];
    int absDelta = delta < 0 ? -delta : delta;
    if (absDelta > MOVE_THRESHOLD_US) {
      if (now - lastChanPrint[i] >= PER_CHANNEL_RATE_MS) {
        Serial.printf("CH%d (idx %d) is changing:  %u → %u  (Δ %+d)\n",
                      i + 1, i, prevChannels[i], channels[i], delta);
        lastChanPrint[i] = now;
      }
      prevChannels[i] = channels[i];
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("test_18_deck_gun_pan — phase 1 discovery");
  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);
  lastHint = millis();
}

void loop() {
  while (ibusSerial.available()) {
    uint8_t b = ibusSerial.read();
    if (ibusIdx == 0 && b != 0x20) continue;
    ibusBuffer[ibusIdx++] = b;
    if (ibusIdx == 32) {
      if (parseIBus()) handleFrame();
      ibusIdx = 0;
    }
  }
}

#endif  // PHASE 1
