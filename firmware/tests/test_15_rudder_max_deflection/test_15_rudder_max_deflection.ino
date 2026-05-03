/*
 * test_15_rudder_max_deflection.ino
 *
 * Find safe servo limits for the rudder by sweeping the stick by hand
 * and recording the most-extreme µs values that were commanded so far.
 *
 * Why: at the full 1000–2000 µs servo range the rudder armature can over-
 * rotate past the linkage's safe arc, flipping the dogbone past center and
 * jamming. We want to clamp the firmware to a tighter range. This test
 * gives us the two numbers (LEFT µs, RIGHT µs) to put in config.h.
 *
 * Procedure:
 *   1. Flash, open Serial Monitor @ 115200, turn on transmitter.
 *   2. Slowly push the stick LEFT until the rudder is at its safe
 *      mechanical extreme. STOP just before any binding/flipping.
 *      Hold there for a beat so the running max captures it.
 *   3. Center, then slowly push the stick RIGHT to its safe extreme.
 *   4. Read the last "MAX so far" line — those two µs values are the
 *      limits to copy into config.h.
 *   5. If you overshoot at any point, type 'r' + ENTER in serial monitor
 *      to reset the running min/max and try again.
 *
 * The ESCs are held at 1500 µs neutral throughout — motors stay stopped.
 *
 * Wiring (same as test_07):
 *   Rudder servo SIGNAL → PCA9685 ch2
 *   Rudder servo V+/GND → PCA9685 V+ rail / GND  (6 V to V+)
 *   Receiver iBUS       → 1 kΩ → GPIO 16 (with 2 kΩ to GND)
 *   PCA9685 SDA/SCL     → GPIO 21 / 22
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
bool pcaPresent = false;

HardwareSerial ibusSerial(1);

// ---- Tunables ----
const uint8_t  RUDDER_CHANNEL_INDEX = 0;       // CH1 = right-stick horizontal (Mode-2 aileron). See NOTES.
const uint8_t  RUDDER_PCA_CHANNEL   = 2;       // PCA9685 channel wired to rudder
const bool     RUDDER_REVERSE       = false;   // flip if left-stick gives right-rudder
const uint16_t REPORT_INTERVAL_MS   = 2000;    // print running max every 2 s

// ---- iBUS state ----
uint8_t  ibusBuffer[32];
uint8_t  ibusIdx       = 0;
uint16_t channels[10];
unsigned long lastFrame = 0;
unsigned long lastWarn  = 0;
bool signalLostWarned   = false;
const unsigned long NO_FRAME_TIMEOUT_MS = 1000;

// ---- Running max state ----
// Values recorded are the µs commanded to the servo (post-clamp, post-reverse),
// because that's what the firmware will eventually clamp.
uint16_t minSeenUs = 1500;   // most-LEFT command (smallest µs)
uint16_t maxSeenUs = 1500;   // most-RIGHT command (largest µs)
bool     gotSignal = false;
unsigned long lastReport = 0;

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
  pca.setPWM(0, 0, usTicks(1500));  // port ESC
  pca.setPWM(1, 0, usTicks(1500));  // starboard ESC
  pca.setPWM(RUDDER_PCA_CHANNEL, 0, usTicks(1500));
}

void resetMaxes() {
  minSeenUs = 1500;
  maxSeenUs = 1500;
  Serial.println();
  Serial.println("[RESET] running min/max cleared. Sweep again.");
  Serial.println();
}

void printReport() {
  int16_t leftDelta  = (int16_t)minSeenUs - 1500;   // negative
  int16_t rightDelta = (int16_t)maxSeenUs - 1500;   // positive
  Serial.printf("MAX so far:  LEFT = %4u µs (Δ %+4d)   RIGHT = %4u µs (Δ %+4d)\n",
                minSeenUs, leftDelta, maxSeenUs, rightDelta);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_15_rudder_max_deflection");
  Serial.println("========================================");
  Serial.printf("Rudder stick: CH%d (iBUS index %d)\n",
                RUDDER_CHANNEL_INDEX + 1, RUDDER_CHANNEL_INDEX);
  Serial.printf("Rudder servo: PCA9685 ch%d  (reverse=%s)\n",
                RUDDER_PCA_CHANNEL, RUDDER_REVERSE ? "true" : "false");
  Serial.println();
  Serial.println("Procedure:");
  Serial.println("  1. Slowly push stick LEFT until rudder is at its safe extreme.");
  Serial.println("     STOP just before binding. Hold for a beat.");
  Serial.println("  2. Center, then slowly push stick RIGHT to its safe extreme.");
  Serial.println("  3. Read the last 'MAX so far' line — those are your limits.");
  Serial.println("  4. Type 'r'+ENTER on serial to reset if you overshoot.");
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
  Serial.println("Waiting for iBUS frames... turn on the transmitter.");
  Serial.println("----------------------------------------");
  lastReport = millis();
}

void handleFrame() {
  uint16_t raw      = channels[RUDDER_CHANNEL_INDEX];
  uint16_t rudderUs = mapRudder(raw);

  if (pcaPresent) {
    pca.setPWM(RUDDER_PCA_CHANNEL, 0, usTicks(rudderUs));
  }

  if (!gotSignal) {
    gotSignal = true;
    Serial.println();
    Serial.printf("[OK] iBUS acquired. Rudder reading %u µs at center.\n", rudderUs);
    Serial.println("Begin sweeping. Reports print every 2 s.");
    Serial.println("----------------------------------------");
  }

  if (rudderUs < minSeenUs) minSeenUs = rudderUs;
  if (rudderUs > maxSeenUs) maxSeenUs = rudderUs;
}

void loop() {
  // Serial reset command
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'r' || c == 'R') resetMaxes();
  }

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

  // Hold rudder centered if frames stop.
  if (gotSignal && lastFrame > 0 &&
      millis() - lastFrame > NO_FRAME_TIMEOUT_MS && !signalLostWarned) {
    if (pcaPresent) pca.setPWM(RUDDER_PCA_CHANNEL, 0, usTicks(1500));
    Serial.println("[WARN] iBUS signal lost — rudder centered. Check transmitter / wiring.");
    signalLostWarned = true;
  }

  if (gotSignal && millis() - lastReport >= REPORT_INTERVAL_MS) {
    printReport();
    lastReport = millis();
  }
}
