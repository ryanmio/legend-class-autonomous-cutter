/*
 * test_19_anchor_winch.ino
 *
 * Forward anchor winch (continuous-rotation servo) on PCA9685 ch9.
 * Knob CH5 (iBUS index 4) commands a binary on/off at the extremes:
 *
 *   Knob ≤ ACTIVATE_LO_US  → lower anchor (slow)
 *   Knob ≥ ACTIVATE_HI_US  → raise anchor (slow)
 *   Anything between        → stop (neutral 1500 µs)
 *
 * Speed is intentionally below full-throttle to protect the anchor chain.
 * Adjust WINCH_LOWER_US / WINCH_RAISE_US to tune — closer to 1500 = slower.
 *
 * Failsafe: winch stops on iBUS loss (no-frame or channel-freeze).
 *
 * Wiring:
 *   Receiver iBUS   → 1 kΩ → GPIO 16 (with 2 kΩ to GND)
 *   Winch servo sig → PCA9685 ch9
 *   PCA9685 SDA/SCL → GPIO 21 / 22
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
bool pcaPresent = false;

HardwareSerial ibusSerial(1);

// ---- Tunables ----
const uint8_t  KNOB_CHANNEL_INDEX = 4;     // CH5 — knob identified in test_18
const uint8_t  WINCH_PCA_CHANNEL  = 9;     // PCA9685 ch9 (one above gun pan ch8)

// Continuous servo: 1500 µs = stop. Distance from 1500 sets speed.
// These values give roughly 40% speed — gentle enough not to snap the chain.
// Lower WINCH_LOWER_US (toward 1000) or raise WINCH_RAISE_US (toward 2000)
// to go faster in each direction.
const uint16_t WINCH_LOWER_US   = 1350;  // lower anchor — slow
const uint16_t WINCH_RAISE_US   = 1650;  // raise anchor — slow
const uint16_t WINCH_STOP_US    = 1500;  // neutral

// Knob thresholds. Knob must be at the hard extreme to activate — anything
// short of the limit is treated as the dead zone and the winch stops.
const uint16_t ACTIVATE_LO_US  = 1050;  // ≤ this → lower
const uint16_t ACTIVATE_HI_US  = 1950;  // ≥ this → raise

const unsigned long REPORT_INTERVAL_MS   = 2000;
const unsigned long FREEZE_TIMEOUT_MS    = 500;
const unsigned long NO_FRAME_TIMEOUT_MS  = 500;

// ---- iBUS state ----
uint8_t  ibusBuffer[32];
uint8_t  ibusIdx = 0;
uint16_t channels[10];
uint16_t prevChannels[10];
bool     prevValid = false;

unsigned long lastFrame         = 0;
unsigned long lastChannelChange = 0;
unsigned long lastWarn          = 0;
unsigned long lastReport        = 0;
bool          gotSignal         = false;

// ---- Failsafe ----
bool failsafeActive = false;

// ---- State for serial reporting ----
enum WinchState { STOPPED, LOWERING, RAISING };
WinchState lastPrinted = STOPPED;

uint16_t usTicks(uint16_t microseconds) {
  return (uint16_t)((microseconds / 20000.0f) * 4096);
}

void writeWinch(uint16_t us) {
  if (!pcaPresent) return;
  pca.setPWM(WINCH_PCA_CHANNEL, 0, usTicks(us));
}

void stopWinch() { writeWinch(WINCH_STOP_US); }

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
  stopWinch();
  Serial.println();
  Serial.printf(">>> FAILSAFE — %s. Winch stopped. <<<\n", reason);
}

void clearFailsafe() {
  if (!failsafeActive) return;
  failsafeActive = false;
  Serial.println(">>> FAILSAFE RECOVERED — knob active again. <<<");
}

void handleFrame() {
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

  if (!failsafeActive && millis() - lastChannelChange > FREEZE_TIMEOUT_MS)
    enterFailsafe("channels frozen (TX off?)");

  if (failsafeActive) return;

  uint16_t knob = channels[KNOB_CHANNEL_INDEX];
  uint16_t cmdUs;
  WinchState state;

  if (knob <= ACTIVATE_LO_US) {
    cmdUs = WINCH_LOWER_US;
    state = LOWERING;
  } else if (knob >= ACTIVATE_HI_US) {
    cmdUs = WINCH_RAISE_US;
    state = RAISING;
  } else {
    cmdUs = WINCH_STOP_US;
    state = STOPPED;
  }

  writeWinch(cmdUs);

  if (!gotSignal) {
    gotSignal = true;
    Serial.println();
    Serial.printf("[OK] iBUS acquired. CH%d = %u µs → winch STOPPED.\n",
                  KNOB_CHANNEL_INDEX + 1, knob);
    Serial.printf("Knob ≤ %u µs → LOWER (%u µs)  |  Knob ≥ %u µs → RAISE (%u µs)\n",
                  ACTIVATE_LO_US, WINCH_LOWER_US, ACTIVATE_HI_US, WINCH_RAISE_US);
    Serial.println("----------------------------------------");
    lastPrinted = STOPPED;
  }

  if (state != lastPrinted) {
    const char* label = (state == LOWERING) ? "LOWERING" :
                        (state == RAISING)  ? "RAISING"  : "STOPPED";
    Serial.printf("Winch: %s  (knob=%u µs  cmd=%u µs)\n", label, knob, cmdUs);
    lastPrinted = state;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_19_anchor_winch");
  Serial.println("========================================");
  Serial.printf("Knob:         CH%d (iBUS idx %d)\n",
                KNOB_CHANNEL_INDEX + 1, KNOB_CHANNEL_INDEX);
  Serial.printf("Winch servo:  PCA9685 ch%d  (continuous rotation)\n", WINCH_PCA_CHANNEL);
  Serial.printf("Lower:  knob ≤ %u µs  →  %u µs\n", ACTIVATE_LO_US, WINCH_LOWER_US);
  Serial.printf("Raise:  knob ≥ %u µs  →  %u µs\n", ACTIVATE_HI_US, WINCH_RAISE_US);
  Serial.printf("Stop :  anything between → 1500 µs\n");
  Serial.println();

  Wire.begin(21, 22);
  Wire.beginTransmission(0x40);
  if (Wire.endTransmission() == 0) {
    pcaPresent = true;
    pca.begin();
    pca.setOscillatorFrequency(27000000);
    pca.setPWMFreq(50);
    Serial.println("[INFO] PCA9685 found — winch stopped at 1500 µs.");
    stopWinch();
  } else {
    Serial.println("[FAIL] PCA9685 not found at 0x40. Check I2C wiring (SDA=21, SCL=22).");
    while (true) delay(1000);
  }

  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);

  Serial.println();
  Serial.println("Waiting for iBUS frames... turn on the transmitter.");
  Serial.println("----------------------------------------");
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

  if (gotSignal && !failsafeActive &&
      millis() - lastFrame > NO_FRAME_TIMEOUT_MS) {
    enterFailsafe("no iBUS frames (RX disconnected?)");
  }
}
