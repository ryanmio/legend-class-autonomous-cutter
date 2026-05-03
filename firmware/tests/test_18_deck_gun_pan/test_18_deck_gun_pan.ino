/*
 * test_18_deck_gun_pan.ino
 *
 * Multi-phase test. Set ACTIVE_PHASE below to swap between modes.
 *
 *   Phase 1: iBUS channel discovery — print which channels move
 *            frame-to-frame. Used to identify the knob (→ CH5) and any
 *            toggle-switch channels we want to use as a weapon-arm gate.
 *
 *   Phase 2: Knob → PCA9685 ch8 pan servo (CONTINUOUS rotation; µs = SPEED).
 *            Naïve passthrough — abandoned because the knob is never centered
 *            at TX power-on so the gun spins immediately. Kept for reference.
 *
 *   Phase 3 (TODO once phase 1 finds a switch channel): toggle switch arms
 *            the pan motor. Switch off → gun forced to 1500 (stopped) regardless
 *            of knob position. Switch on → knob controls pan with a wide
 *            deadband (~±100 µs) so only deliberate pushes slew the gun.
 *            Operator workflow: power on, center knob deliberately, then arm.
 *
 * The deck gun pan servo is a CONTINUOUS-ROTATION servo:
 *   1500 µs        → stop
 *   1500 + N µs    → rotate one way at speed ∝ N
 *   1500 - N µs    → rotate the other way at speed ∝ N
 * No position feedback — knob commands SLEW SPEED, not angle.
 *
 * NOTE: config.h:63 says CH_GUN_PAN 3 — pre-hardware scaffold value. Real
 * wiring is PCA9685 ch8. Update config.h once a working phase passes.
 *
 * Wiring:
 *   Receiver iBUS    → 1 kΩ → GPIO 16 (with 2 kΩ to GND)
 *   Pan servo signal → PCA9685 ch8       (phase 2 / 3 only)
 *   PCA9685 SDA/SCL  → GPIO 21 / 22      (phase 2 / 3 only)
 */

// =============================================================================
// PHASE SELECTOR — set to 1 for discovery, 2 for naive knob → pan, 3 for
// toggle-armed knob → pan. Recompile after changing.
// =============================================================================
#define ACTIVE_PHASE 1

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

#if ACTIVE_PHASE == 2
// =============================================================================
// PHASE 2 — naive knob → pan (DEPRECATED: gun spins on TX power-on)
// =============================================================================

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
bool pcaPresent = false;

HardwareSerial ibusSerial(1);

// ---- Tunables ----
const uint8_t  KNOB_CHANNEL_INDEX   = 4;     // CH5, found in phase 1
const uint8_t  GUN_PAN_PCA_CHANNEL  = 8;     // PCA9685 ch8 (real wiring)
// CONTINUOUS servo: these are SPEED limits, not position limits.
// 1500 = stop; 1300 = full speed CCW (or CW, depending on PAN_REVERSE).
const uint16_t PAN_MIN_US           = 1300;  // max speed one direction
const uint16_t PAN_MAX_US           = 1700;  // max speed the other direction
const uint16_t PAN_DEADBAND_US      = 30;    // |knob - 1500| ≤ this → force output 1500 (no creep)
const bool     PAN_REVERSE          = false; // flip if knob CW spins gun CCW

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
  // Center deadband: knob within ±PAN_DEADBAND_US of 1500 → exact stop.
  // Continuous servos creep at the nominal 1500 µs "stop" pulse due to
  // factory trim. Snapping to 1500 only when actually centered eliminates
  // the creep without sacrificing fine speed control elsewhere.
  int d = (int)rawUs - 1500;
  if (d < 0) d = -d;
  if (d <= (int)PAN_DEADBAND_US) return 1500;
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
  Serial.printf("Knob:      CH%d (idx %d)\n",
                KNOB_CHANNEL_INDEX + 1, KNOB_CHANNEL_INDEX);
  Serial.printf("Pan servo: PCA9685 ch%d  (CONTINUOUS rotation — µs = SPEED, not angle)\n",
                GUN_PAN_PCA_CHANNEL);
  Serial.printf("Speed:     %u..%u µs   deadband ±%u µs   reverse=%s\n",
                PAN_MIN_US, PAN_MAX_US, PAN_DEADBAND_US,
                PAN_REVERSE ? "true" : "false");
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


#endif  // ACTIVE_PHASE == 2


// ============================================================================
// PHASE 1 — iBUS channel discovery (sticks, knobs, switches).
// ============================================================================
#if ACTIVE_PHASE == 1

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
  Serial.println("========================================");
  Serial.println("  test_18 — phase 1: iBUS channel discovery");
  Serial.println("========================================");
  Serial.println("Goal: identify which channels are mapped to the toggle");
  Serial.println("switches (and any other unused control on the TX).");
  Serial.println();
  Serial.println("Procedure:");
  Serial.println("  1. Turn on transmitter, hands off the sticks.");
  Serial.println("  2. Wait for [OK] iBUS frames arriving.");
  Serial.println("  3. Flip ONE switch at a time. Watch for 'CHn is changing'.");
  Serial.println("  4. Note which channel each switch maps to (and observed values).");
  Serial.println("     A two-position switch typically jumps 1000 ↔ 2000 µs.");
  Serial.println("     A three-position switch shows 1000 / 1500 / 2000.");
  Serial.println("  5. Also confirm CH5 still moves with the right knob.");
  Serial.println();
  Serial.println("If a switch produces NO output, it isn't mapped to a channel");
  Serial.println("on the TX. Map it through the FS-i6X menu (Aux. channels) first.");
  Serial.println();
  Serial.printf("Move threshold: %u µs.\n", MOVE_THRESHOLD_US);
  Serial.println();
  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);
  Serial.println("Waiting for iBUS frames...");
  Serial.println("----------------------------------------");
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

#endif  // ACTIVE_PHASE == 1
