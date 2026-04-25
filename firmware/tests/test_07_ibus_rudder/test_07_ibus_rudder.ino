/*
 * test_07_ibus_rudder.ino
 *
 * BENCH/HULL TEST — closes the manual control loop:
 *   transmitter stick → ESP32 (iBUS) → PCA9685 → rudder servo.
 *
 * Three pass gates:
 *   PASS 1/3  iBUS signal acquired, rudder holding center
 *   PASS 2/3  Full LEFT rudder reached (channel ≤ 1100 µs)
 *   PASS 3/3  Full RIGHT rudder reached (channel ≥ 1900 µs)
 *
 * After all three pass the operator visually confirms:
 *   - response is smooth and proportional
 *   - direction is correct (left stick → left rudder)
 *   - linkage doesn't bind at the extremes
 *
 * Wiring:
 *   Rudder servo SIGNAL → PCA9685 ch2
 *   Rudder servo V+/GND → PCA9685 V+ rail / GND  (6V to V+)
 *   Receiver iBUS       → 1kΩ → GPIO 16 (with 2kΩ to GND)
 *   PCA9685 SDA/SCL     → GPIO 21 / 22
 *
 * If direction is reversed at the bench, set RUDDER_REVERSE = true and
 * re-flash. Otherwise flip the servo horn 180° on the splines.
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
bool pcaPresent = false;

HardwareSerial ibusSerial(1);

// ---- Tunables ----
const uint8_t  RUDDER_CHANNEL_INDEX = 0;       // CH1 = right-stick horizontal (Mode-2 aileron)
const uint8_t  RUDDER_PCA_CHANNEL   = 2;       // PCA9685 channel wired to rudder
const bool     RUDDER_REVERSE       = false;   // flip if left-stick gives right-rudder
const uint16_t LEFT_PASS_US         = 1100;    // ≤ this counts as full left
const uint16_t RIGHT_PASS_US        = 1900;    // ≥ this counts as full right

// ---- iBUS state ----
uint8_t  ibusBuffer[32];
uint8_t  ibusIdx       = 0;
uint16_t channels[10];
unsigned long lastFrame = 0;
unsigned long lastWarn  = 0;

// ---- Test state ----
enum TestState { WAIT_SIGNAL, WAIT_LEFT, WAIT_RIGHT, ALL_PASSED };
TestState state = WAIT_SIGNAL;
bool gotLeft  = false;
bool gotRight = false;
unsigned long lastHint = 0;
const unsigned long HINT_INTERVAL_MS    = 10000;
const unsigned long NO_FRAME_TIMEOUT_MS = 1000;
bool signalLostWarned = false;

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
  return true;
}

uint16_t mapRudder(uint16_t raw) {
  // Clamp to a safe servo range and optionally reverse around 1500.
  if (raw < 1000) raw = 1000;
  if (raw > 2000) raw = 2000;
  if (RUDDER_REVERSE) raw = 3000 - raw;
  return raw;
}

void writeNeutral() {
  if (!pcaPresent) return;
  pca.setPWM(0, 0, usTicks(1500));  // port ESC — held neutral throughout
  pca.setPWM(1, 0, usTicks(1500));  // starboard ESC — held neutral throughout
  pca.setPWM(RUDDER_PCA_CHANNEL, 0, usTicks(1500));
}

void printHint() {
  switch (state) {
    case WAIT_SIGNAL:
      Serial.println("  ...waiting for iBUS frames. Turn on your transmitter.");
      break;
    case WAIT_LEFT:
      Serial.println("  ...push rudder stick FULL LEFT.");
      break;
    case WAIT_RIGHT:
      Serial.println("  ...push rudder stick FULL RIGHT.");
      break;
    case ALL_PASSED:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_07_ibus_rudder");
  Serial.println("========================================");
  Serial.printf("Rudder stick: CH%d (iBUS index %d)\n",
                RUDDER_CHANNEL_INDEX + 1, RUDDER_CHANNEL_INDEX);
  Serial.printf("Rudder servo: PCA9685 ch%d  (reverse=%s)\n",
                RUDDER_PCA_CHANNEL, RUDDER_REVERSE ? "true" : "false");
  Serial.println();

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
    Serial.println("[INFO] ESCs armed. Rudder centered.");
  } else {
    Serial.println("[FAIL] PCA9685 not found at 0x40. Check I2C wiring (SDA=21, SCL=22).");
    while (true) delay(1000);
  }

  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);

  Serial.println();
  Serial.println("Step 1: turn on your transmitter and center the rudder stick.");
  Serial.println("----------------------------------------");
  lastHint = millis();
}

void handleFrame() {
  uint16_t raw      = channels[RUDDER_CHANNEL_INDEX];
  uint16_t rudderUs = mapRudder(raw);

  // Always drive the rudder servo from the live stick, regardless of state —
  // the operator needs to see the response to decide if it's correct.
  if (pcaPresent) {
    pca.setPWM(RUDDER_PCA_CHANNEL, 0, usTicks(rudderUs));
  }

  switch (state) {
    case WAIT_SIGNAL:
      Serial.println();
      Serial.println("PASS (1/3): iBUS signal acquired, rudder centered.");
      Serial.printf("  Rudder channel reads %d µs.\n", raw);
      Serial.println("Step 2: push the rudder stick FULL LEFT.");
      Serial.println("----------------------------------------");
      state = WAIT_LEFT;
      lastHint = millis();
      break;

    case WAIT_LEFT:
      if (raw <= LEFT_PASS_US) {
        gotLeft = true;
        Serial.println();
        Serial.printf("PASS (2/3): full LEFT reached (CH%d=%d µs).\n",
                      RUDDER_CHANNEL_INDEX + 1, raw);
        Serial.println("Step 3: push the rudder stick FULL RIGHT.");
        Serial.println("----------------------------------------");
        state = WAIT_RIGHT;
        lastHint = millis();
      }
      break;

    case WAIT_RIGHT:
      if (raw >= RIGHT_PASS_US) {
        gotRight = true;
        Serial.println();
        Serial.printf("PASS (3/3): full RIGHT reached (CH%d=%d µs).\n",
                      RUDDER_CHANNEL_INDEX + 1, raw);
        Serial.println();
        Serial.println("========================================");
        Serial.println("  RANGE TEST PASSED");
        Serial.println("========================================");
        Serial.println("Now visually verify on the bench / in the hull:");
        Serial.println("  [ ] response is smooth and proportional");
        Serial.println("  [ ] direction is correct (left stick = left rudder)");
        Serial.println("  [ ] linkage moves freely at extremes (no binding)");
        Serial.println();
        Serial.println("If direction is reversed: set RUDDER_REVERSE=true and re-flash,");
        Serial.println("or rotate the servo horn 180° on the splines.");
        Serial.println("If anything binds: trim LEFT_PASS_US / RIGHT_PASS_US or shorten linkage.");
        Serial.println();
        Serial.println("Rudder remains live — keep moving the stick to verify.");
        state = ALL_PASSED;
      }
      break;

    case ALL_PASSED:
      // Live passthrough only — no further serial output.
      break;
  }
}

void loop() {
  while (ibusSerial.available()) {
    uint8_t b = ibusSerial.read();
    if (ibusIdx == 0 && b != 0x20) continue;
    ibusBuffer[ibusIdx++] = b;
    if (ibusIdx == 32) {
      if (parseIBus()) {
        handleFrame();
        signalLostWarned = false;
      }
      ibusIdx = 0;
    }
  }

  // If frames stop entirely, hold rudder at center and warn once.
  if (state != WAIT_SIGNAL && lastFrame > 0 &&
      millis() - lastFrame > NO_FRAME_TIMEOUT_MS && !signalLostWarned) {
    if (pcaPresent) pca.setPWM(RUDDER_PCA_CHANNEL, 0, usTicks(1500));
    Serial.println("[WARN] iBUS signal lost — rudder centered. Check transmitter / wiring.");
    signalLostWarned = true;
  }

  if (state != ALL_PASSED && millis() - lastHint > HINT_INTERVAL_MS) {
    printHint();
    lastHint = millis();
  }
}
