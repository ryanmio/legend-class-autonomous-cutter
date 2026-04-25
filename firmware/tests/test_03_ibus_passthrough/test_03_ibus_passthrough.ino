/*
 * test_03_ibus_passthrough.ino
 *
 * BENCH TEST — iBUS chain verification. Walks through three pass gates,
 * prints PASS as each is satisfied, then goes silent.
 *
 *   PASS 1/3  iBUS frames arriving on GPIO 16 with valid checksums
 *   PASS 2/3  Channel values respond to stick movement
 *   PASS 3/3  Failsafe triggers (channel-freeze OR no-frame for 500 ms)
 *
 * Flysky receivers do NOT stop sending frames on signal loss — they keep
 * streaming valid frames with frozen failsafe-preset values. Detect this by
 * watching channel values, not frame timing.
 *
 * Voltage divider REQUIRED on iBUS wire before GPIO 16:
 *   Receiver iBUS → 1kΩ → GPIO 16 → 2kΩ → GND
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
bool pcaPresent = false;

HardwareSerial ibusSerial(1);  // UART1

uint8_t  ibusBuffer[32];
uint8_t  ibusIdx      = 0;
uint16_t channels[10];
uint16_t baselineChannels[10];
uint16_t prevChannels[10];

enum TestState { WAIT_SIGNAL, WAIT_STICKS, WAIT_FAILSAFE, ALL_PASSED };
TestState state = WAIT_SIGNAL;

unsigned long lastFrame         = 0;
unsigned long lastChannelChange = 0;
unsigned long lastHint          = 0;
unsigned long lastWarn          = 0;
unsigned long frameCount        = 0;

const unsigned long HINT_INTERVAL_MS    = 10000;  // one quiet hint every 10s while waiting
const unsigned long FREEZE_TIMEOUT_MS   = 500;    // channels frozen this long → failsafe
const unsigned long NO_FRAME_TIMEOUT_MS = 500;    // no frames at all this long → failsafe
const uint16_t      STICK_MOVE_THRESHOLD = 50;    // µs deviation from baseline = "moved"

uint16_t usTicks(uint16_t microseconds) {
  return (uint16_t)((microseconds / 20000.0f) * 4096);
}

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
  frameCount++;
  return true;
}

void writeNeutral() {
  if (!pcaPresent) return;
  pca.setPWM(0, 0, usTicks(1500));
  pca.setPWM(1, 0, usTicks(1500));
  pca.setPWM(2, 0, usTicks(1500));
}

void writePassthrough() {
  if (!pcaPresent) return;
  pca.setPWM(0, 0, usTicks(channels[2]));  // throttle → port ESC
  pca.setPWM(2, 0, usTicks(channels[3]));  // yaw → rudder
}

void printHint() {
  switch (state) {
    case WAIT_SIGNAL:
      Serial.println("  ...waiting for iBUS frames. Turn on your transmitter.");
      break;
    case WAIT_STICKS:
      Serial.println("  ...waiting for stick movement on the transmitter.");
      break;
    case WAIT_FAILSAFE:
      Serial.println("  ...waiting for failsafe. Turn OFF your transmitter.");
      break;
    case ALL_PASSED:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_03_ibus_passthrough");
  Serial.println("========================================");

  Wire.begin(21, 22);
  Wire.beginTransmission(0x40);
  if (Wire.endTransmission() == 0) {
    pcaPresent = true;
    pca.begin();
    pca.setOscillatorFrequency(27000000);
    pca.setPWMFreq(50);
    Serial.println("[INFO] PCA9685 found — arming ESCs at 1500 µs (3 s delay)...");
    writeNeutral();
    delay(3000);
    Serial.println("[INFO] ESCs armed.");
  } else {
    Serial.println("[INFO] No PCA9685 — running iBUS-only.");
  }

  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);  // RX=GPIO16

  Serial.println();
  Serial.println("Step 1: turn on your transmitter.");
  Serial.println("----------------------------------------");
  lastHint = millis();
}

void handleFrame() {
  switch (state) {
    case WAIT_SIGNAL:
      Serial.println();
      Serial.println("PASS (1/3): iBUS signal acquired, checksums OK.");
      Serial.println("Step 2: move any stick on your transmitter.");
      Serial.println("----------------------------------------");
      for (int i = 0; i < 10; i++) baselineChannels[i] = channels[i];
      state = WAIT_STICKS;
      lastHint = millis();
      break;

    case WAIT_STICKS: {
      bool moved = false;
      for (int i = 0; i < 10; i++) {
        int d = (int)channels[i] - (int)baselineChannels[i];
        if (d < 0) d = -d;
        if (d > STICK_MOVE_THRESHOLD) { moved = true; break; }
      }
      writePassthrough();
      if (moved) {
        Serial.println();
        Serial.println("PASS (2/3): channel values respond to stick input.");
        Serial.println("Step 3: TURN OFF your transmitter to test failsafe.");
        Serial.println("----------------------------------------");
        state = WAIT_FAILSAFE;
        lastChannelChange = millis();
        lastHint = millis();
      }
      break;
    }

    case WAIT_FAILSAFE: {
      bool anyDiff = false;
      for (int i = 0; i < 10; i++) {
        if (channels[i] != prevChannels[i]) { anyDiff = true; break; }
      }
      if (anyDiff) lastChannelChange = millis();
      writePassthrough();
      if (millis() - lastChannelChange > FREEZE_TIMEOUT_MS) {
        writeNeutral();
        Serial.println();
        Serial.println("PASS (3/3): failsafe detected (channels frozen).");
        Serial.println();
        Serial.println("========================================");
        Serial.println("  ALL TESTS PASSED — iBUS chain verified");
        Serial.println("========================================");
        state = ALL_PASSED;
      }
      break;
    }

    case ALL_PASSED:
      writeNeutral();
      break;
  }
  for (int i = 0; i < 10; i++) prevChannels[i] = channels[i];
}

void loop() {
  // ---- Read and parse iBUS frames ----
  while (ibusSerial.available()) {
    uint8_t b = ibusSerial.read();
    if (ibusIdx == 0 && b != 0x20) continue;
    ibusBuffer[ibusIdx++] = b;
    if (ibusIdx == 32) {
      if (parseIBus()) handleFrame();
      ibusIdx = 0;
    }
  }

  // ---- Secondary failsafe path: receiver disconnected entirely ----
  // Only meaningful in WAIT_FAILSAFE — counts as PASS 3/3.
  if (state == WAIT_FAILSAFE && millis() - lastFrame > NO_FRAME_TIMEOUT_MS) {
    writeNeutral();
    Serial.println();
    Serial.println("PASS (3/3): failsafe detected (receiver lost frames).");
    Serial.println();
    Serial.println("========================================");
    Serial.println("  ALL TESTS PASSED — iBUS chain verified");
    Serial.println("========================================");
    state = ALL_PASSED;
  }

  // ---- One quiet hint every 10 s while waiting ----
  if (state != ALL_PASSED && millis() - lastHint > HINT_INTERVAL_MS) {
    printHint();
    lastHint = millis();
  }
}
