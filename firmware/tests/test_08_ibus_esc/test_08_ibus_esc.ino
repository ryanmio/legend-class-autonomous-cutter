/*
 * test_08_ibus_esc.ino
 *
 * BENCH/HULL TEST — closes the throttle control loop:
 *   transmitter throttle stick → ESP32 (iBUS) → PCA9685 → both ESCs → motors.
 *
 * Three pass gates (auto-verified):
 *   PASS 1/3  iBUS signal acquired AND throttle at neutral → ESCs ARMED
 *   PASS 2/3  Throttle stick advanced past ADVANCE_THRESHOLD_US
 *   PASS 3/3  Throttle returned to neutral cleanly
 *
 * After all three pass, sketch enters LIVE mode: throttle drives both motors
 * with full failsafe protection.
 *
 * SAFETY:
 *   * PROPS OFF, or boat firmly secured so it cannot launch off the bench.
 *   * Output is clamped to MAX_FWD_US (default 1800 µs ≈ 60% forward) for the
 *     entire test. Reverse is clamped off unless ALLOW_REVERSE = true.
 *   * Motors stay at 1500 µs (stop) until the operator confirms throttle is
 *     centered after boot. Throttle non-zero at startup will NOT spin motors.
 *   * On failsafe (channels frozen OR no frames), motors are forced to 1500
 *     and the sketch drops back to the arm gate — operator must re-center
 *     throttle to re-arm.
 *
 * Failure modes detected and logged:
 *   * iBUS signal not arriving       → no PASS 1/3, hint every 10 s
 *   * Throttle non-zero at startup   → "ARM BLOCKED" warning, motors at 1500
 *   * Checksum errors                → rate-limited [WARN]
 *   * Channel value out of range     → [WARN] once per occurrence
 *   * TX off / receiver disconnected → FAILSAFE → motors stopped, must re-arm
 *
 * Wiring:
 *   Port ESC signal       → PCA9685 ch0
 *   Starboard ESC signal  → PCA9685 ch1
 *   ESC BEC/ground        → PCA9685 GND rail (ESCs are powered from battery)
 *   Receiver iBUS         → 1kΩ → GPIO 16 (with 2kΩ to GND)
 *   PCA9685 SDA / SCL     → GPIO 21 / 22
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
bool pcaPresent = false;

HardwareSerial ibusSerial(1);

// ---- Tunables ----
const uint8_t  THROTTLE_CHANNEL_INDEX = 2;       // CH3 = throttle (left-stick vertical)
const uint8_t  PORT_ESC_PCA_CHANNEL   = 0;
const uint8_t  STBD_ESC_PCA_CHANNEL   = 1;
const bool     ALLOW_REVERSE          = false;   // allow stick below 1500 to drive reverse
const uint16_t MAX_FWD_US             = 1800;    // forward cap (60% of 1500→2000)
const uint16_t MIN_REV_US             = 1200;    // reverse cap (only used if ALLOW_REVERSE)
const uint16_t NEUTRAL_DEADBAND_US    = 30;      // ±this around 1500 counts as neutral
const uint16_t ADVANCE_THRESHOLD_US   = 1700;    // throttle must reach this for PASS 2/3

// ---- iBUS state ----
uint8_t  ibusBuffer[32];
uint8_t  ibusIdx = 0;
uint16_t channels[10];
uint16_t prevChannels[10];
bool     prevValid = false;
unsigned long lastFrame         = 0;
unsigned long lastChannelChange = 0;
unsigned long lastWarn          = 0;
bool     rangeWarned            = false;

// ---- Test state ----
enum TestState {
  WAIT_SIGNAL,
  WAIT_NEUTRAL,   // arm gate — motors held at 1500 until throttle centered
  WAIT_ADVANCE,   // armed; waiting for throttle to push forward
  WAIT_RETURN,    // saw advance; waiting for return to neutral
  LIVE            // all three gates passed
};
TestState state = WAIT_SIGNAL;

bool gate1_done = false;
bool gate2_done = false;
bool gate3_done = false;
bool failsafeActive = false;

unsigned long lastHint = 0;
const unsigned long HINT_INTERVAL_MS    = 10000;
const unsigned long FREEZE_TIMEOUT_MS   = 500;
const unsigned long NO_FRAME_TIMEOUT_MS = 500;

uint16_t usTicks(uint16_t microseconds) {
  return (uint16_t)((microseconds / 20000.0f) * 4096);
}

bool atNeutral(uint16_t stickUs) {
  int d = (int)stickUs - 1500;
  if (d < 0) d = -d;
  return d <= NEUTRAL_DEADBAND_US;
}

uint16_t computeOutput(uint16_t stickUs) {
  // Default to neutral whenever we are not authorized to drive motors.
  if (failsafeActive)                    return 1500;
  if (state == WAIT_SIGNAL)              return 1500;
  if (state == WAIT_NEUTRAL)             return 1500;
  // Armed: passthrough, clamped to safe range.
  uint16_t out = stickUs;
  if (out > MAX_FWD_US) out = MAX_FWD_US;
  if (ALLOW_REVERSE) {
    if (out < MIN_REV_US) out = MIN_REV_US;
  } else {
    if (out < 1500) out = 1500;
  }
  return out;
}

void writeESCs(uint16_t out) {
  if (!pcaPresent) return;
  pca.setPWM(PORT_ESC_PCA_CHANNEL, 0, usTicks(out));
  pca.setPWM(STBD_ESC_PCA_CHANNEL, 0, usTicks(out));
}

void writeNeutral() { writeESCs(1500); }

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
  Serial.printf(">>> FAILSAFE — %s. Motors stopped. <<<\n", reason);
  Serial.println("Re-center throttle once signal returns to re-arm.");
  // Disarm: drop state back to the neutral check.
  if (state != WAIT_SIGNAL) state = WAIT_NEUTRAL;
  lastHint = millis();
}

void clearFailsafe() {
  if (!failsafeActive) return;
  failsafeActive = false;
  Serial.println();
  Serial.println(">>> FAILSAFE RECOVERED — re-center throttle to re-arm. <<<");
  lastHint = millis();
}

void printHint() {
  if (failsafeActive) return;  // failsafe message itself is loud enough
  switch (state) {
    case WAIT_SIGNAL:
      Serial.println("  ...waiting for iBUS frames. Turn on your transmitter.");
      break;
    case WAIT_NEUTRAL:
      Serial.printf("  ...waiting for throttle at neutral (CH%d=%d µs, need 1500 ±%d).\n",
                    THROTTLE_CHANNEL_INDEX + 1,
                    channels[THROTTLE_CHANNEL_INDEX],
                    NEUTRAL_DEADBAND_US);
      break;
    case WAIT_ADVANCE:
      Serial.println("  ...ADVANCE the throttle stick forward to spin the motors.");
      break;
    case WAIT_RETURN:
      Serial.println("  ...return throttle to neutral.");
      break;
    case LIVE:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_08_ibus_esc");
  Serial.println("========================================");
  Serial.println("SAFETY: PROPS OFF or boat firmly secured.");
  Serial.printf("Throttle stick: CH%d (iBUS index %d)\n",
                THROTTLE_CHANNEL_INDEX + 1, THROTTLE_CHANNEL_INDEX);
  Serial.printf("ESCs: PCA9685 ch%d (port) + ch%d (starboard)\n",
                PORT_ESC_PCA_CHANNEL, STBD_ESC_PCA_CHANNEL);
  Serial.printf("Limits: forward cap %d µs, reverse %s\n",
                MAX_FWD_US, ALLOW_REVERSE ? "ENABLED" : "disabled");
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
    Serial.println("[INFO] ESCs armed at hardware level. Motors will NOT spin until");
    Serial.println("       PASS 1/3 — throttle stick confirmed at neutral.");
  } else {
    Serial.println("[FAIL] PCA9685 not found at 0x40. Check I2C wiring (SDA=21, SCL=22).");
    while (true) delay(1000);
  }

  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);

  Serial.println();
  Serial.println("Step 1: turn on TX, center throttle stick (CH3 = left-stick vertical).");
  Serial.println("----------------------------------------");
  lastHint = millis();
}

void handleFrame() {
  // ---- Channel-freeze detection (Flysky failsafe) ----
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

  if (!failsafeActive && state != WAIT_SIGNAL &&
      millis() - lastChannelChange > FREEZE_TIMEOUT_MS) {
    enterFailsafe("channels frozen (TX off?)");
  }

  // ---- Range sanity check on throttle channel ----
  uint16_t throttleStick = channels[THROTTLE_CHANNEL_INDEX];
  if ((throttleStick < 900 || throttleStick > 2100) && !rangeWarned) {
    Serial.printf("[WARN] throttle channel out of range (%d µs). Check TX endpoints.\n",
                  throttleStick);
    rangeWarned = true;
  } else if (throttleStick >= 900 && throttleStick <= 2100) {
    rangeWarned = false;
  }

  // ---- State machine ----
  switch (state) {
    case WAIT_SIGNAL:
      Serial.println();
      Serial.println(">>> SIGNAL ACQUIRED <<<");
      Serial.printf("  Throttle channel reads %d µs.\n", throttleStick);
      if (!atNeutral(throttleStick)) {
        Serial.println("  [ARM BLOCKED] throttle is not at neutral. Center the stick.");
      }
      Serial.println("----------------------------------------");
      lastChannelChange = millis();
      state = WAIT_NEUTRAL;
      lastHint = millis();
      break;

    case WAIT_NEUTRAL:
      if (!failsafeActive && atNeutral(throttleStick)) {
        if (!gate1_done) {
          gate1_done = true;
          Serial.println();
          Serial.println("PASS (1/3): throttle at neutral. ESCs ARMED — motors are LIVE.");
          Serial.println("Step 2: ADVANCE throttle stick forward (motors should spin).");
          Serial.println("----------------------------------------");
        } else {
          Serial.println();
          Serial.println(">>> RE-ARMED — throttle at neutral, motors live. <<<");
          Serial.println("----------------------------------------");
        }
        if (!gate2_done)      state = WAIT_ADVANCE;
        else if (!gate3_done) state = WAIT_RETURN;
        else                  state = LIVE;
        lastHint = millis();
      }
      break;

    case WAIT_ADVANCE:
      if (throttleStick >= ADVANCE_THRESHOLD_US) {
        gate2_done = true;
        Serial.println();
        Serial.printf("PASS (2/3): throttle advanced (CH%d=%d µs, output capped at %d µs).\n",
                      THROTTLE_CHANNEL_INDEX + 1, throttleStick, MAX_FWD_US);
        Serial.println("Step 3: return throttle to neutral.");
        Serial.println("----------------------------------------");
        state = WAIT_RETURN;
        lastHint = millis();
      }
      break;

    case WAIT_RETURN:
      if (atNeutral(throttleStick)) {
        gate3_done = true;
        Serial.println();
        Serial.printf("PASS (3/3): throttle returned to neutral (%d µs). Motors stopped.\n",
                      throttleStick);
        Serial.println();
        Serial.println("========================================");
        Serial.println("  RANGE TEST PASSED");
        Serial.println("========================================");
        Serial.println("Visually verify on the bench:");
        Serial.println("  [ ] BOTH motors spun during throttle advance");
        Serial.println("  [ ] motors spun in the SAME direction (or matched if reversed by wiring)");
        Serial.println("  [ ] no excessive ESC heat or smell");
        Serial.println("  [ ] motors stopped cleanly when stick returned to center");
        Serial.println();
        Serial.println("Sketch is now LIVE — throttle drives both motors continuously.");
        Serial.println("Failsafe is active. Cut TX power to verify auto-stop.");
        state = LIVE;
        lastHint = millis();
      }
      break;

    case LIVE:
      // free-run; no further serial output
      break;
  }

  // ---- Drive ESCs ----
  writeESCs(computeOutput(throttleStick));
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

  // ---- Secondary failsafe: no frames at all ----
  if (state != WAIT_SIGNAL && !failsafeActive &&
      millis() - lastFrame > NO_FRAME_TIMEOUT_MS) {
    enterFailsafe("no iBUS frames (RX disconnected?)");
  }

  // ---- Periodic hint while waiting ----
  if (state != LIVE && millis() - lastHint > HINT_INTERVAL_MS) {
    printHint();
    lastHint = millis();
  }
}
