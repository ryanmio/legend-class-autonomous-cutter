/*
 * test_17_manual_sailing.ino
 *
 * BENCH/HULL TEST — full manual-control loop for sailing the boat by stick.
 * Closes:
 *   transmitter sticks → ESP32 (iBUS) → PCA9685 → both ESCs + rudder servo,
 * with differential-thrust mixing and a reverse-interlock safety.
 *
 * STICK SCHEME (Mode-2 with center-sprung throttle):
 *   CH1  right-stick H   → rudder
 *   CH2  right-stick V   → reverse command (down = more reverse)
 *   CH3  left-stick V    → forward throttle  AND  reverse-enable interlock
 *
 * Reverse only engages when BOTH:
 *   - left stick is pushed all the way down (≤ LEFT_STICK_DOWN_US)
 *   - right stick V is below neutral (CH2 < 1500 - dead band)
 * One stick alone never produces reverse output. This prevents an idle
 * left stick from accidentally allowing reverse, and prevents a bumped
 * right stick from kicking the motors backwards.
 *
 * Five pass gates (auto-verified):
 *   PASS 1/5  iBUS acquired AND all sticks at safe neutral → ARMED
 *   PASS 2/5  Rudder swept LEFT past RUDDER_LEFT_THRESHOLD_US
 *   PASS 3/5  Rudder swept RIGHT past RUDDER_RIGHT_THRESHOLD_US
 *   PASS 4/5  Forward throttle advanced WITH rudder offset → port and
 *             starboard ESC outputs differ by ≥ DIFF_MIN_SPLIT_US
 *             (proves the differential-thrust mix is actually splitting)
 *   PASS 5/5  Reverse interlock verified — Phase A: right-stick down with
 *             left stick centered must NOT drive reverse (output stays at
 *             1500). Phase B: left stick fully down + right stick down DOES
 *             drive reverse.
 *
 * After all five pass and operator returns sticks to neutral, sketch enters
 * LIVE mode for free-form sailing. Failsafe (channel-freeze) is active in
 * every state once signal is acquired.
 *
 * SAFETY:
 *   * PROPS OFF, or boat firmly secured so it cannot launch off the bench.
 *   * Forward output capped at MAX_FWD_US (1800 µs) — the ESC never sees
 *     more than that even at full stick. Stick reading is preserved for
 *     telemetry; the cap is applied to the µs sent to the ESC only.
 *   * Reverse output capped at MIN_REV_US (1200 µs).
 *   * Rudder clamped to RUDDER_MIN_US..RUDDER_MAX_US (1330..1670, from
 *     test_15). Past those bounds the dogbone linkage flips center and jams.
 *   * On failsafe (channels frozen), all three outputs forced to neutral
 *     and sketch disarms back to the safe-neutral check — operator must
 *     re-center every stick to re-arm.
 *
 * Wiring (same as test_08 + rudder from test_07):
 *   Port ESC signal      → PCA9685 ch0
 *   Starboard ESC signal → PCA9685 ch1
 *   Rudder servo signal  → PCA9685 ch2
 *   Receiver iBUS        → 1 kΩ → GPIO 16 (with 2 kΩ to GND)
 *   PCA9685 SDA / SCL    → GPIO 21 / 22
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
bool pcaPresent = false;

HardwareSerial ibusSerial(1);

// ---- Channel mapping ----
const uint8_t  RUDDER_CHANNEL_INDEX     = 0;   // CH1 right-stick H
const uint8_t  REVERSE_CHANNEL_INDEX    = 1;   // CH2 right-stick V (down = reverse)
const uint8_t  THROTTLE_CHANNEL_INDEX   = 2;   // CH3 left-stick V (forward + interlock)

// ---- PCA9685 channels ----
const uint8_t  PORT_ESC_PCA_CHANNEL     = 0;
const uint8_t  STBD_ESC_PCA_CHANNEL     = 1;
const uint8_t  RUDDER_PCA_CHANNEL       = 2;

// ---- Output limits ----
const uint16_t RUDDER_MIN_US            = 1330;   // from test_15
const uint16_t RUDDER_MAX_US            = 1670;
const uint16_t MAX_FWD_US               = 1800;   // ~60% forward cap
const uint16_t MIN_REV_US               = 1200;   // ~60% reverse cap
const bool     RUDDER_REVERSE           = false;

// ---- Differential thrust ----
// Mirrors the production formula in motors.cpp:46. Apply same as in nav code
// so this test exercises the real mixing math, not a stand-in.
const float    DIFF_THRUST_FACTOR       = 0.3f;

// ---- Safety thresholds ----
const uint16_t NEUTRAL_DEADBAND_US      = 30;     // stick within 1500±this counts as centered
const uint16_t LEFT_STICK_DOWN_US       = 1100;   // left stick "all the way down" interlock
const uint16_t REVERSE_DEADBAND_US      = 30;     // right-stick-V below 1500-this = reverse intent

// ---- Gate thresholds ----
const uint16_t RUDDER_LEFT_THRESHOLD_US  = 1400;  // rudder µs ≤ this passes left sweep
const uint16_t RUDDER_RIGHT_THRESHOLD_US = 1600;  // rudder µs ≥ this passes right sweep
const uint16_t FWD_ADVANCE_THRESHOLD_US  = 1700;  // left stick must reach this with rudder offset
const uint16_t RUDDER_OFFSET_FOR_DIFF_US = 50;    // |rudder - 1500| must exceed this during fwd
const uint16_t DIFF_MIN_SPLIT_US         = 30;    // |port - stbd| must exceed this to count

// ---- iBUS state ----
uint8_t  ibusBuffer[32];
uint8_t  ibusIdx = 0;
uint16_t channels[10];
uint16_t prevChannels[10];
bool     prevValid = false;
unsigned long lastFrame         = 0;
unsigned long lastChannelChange = 0;
unsigned long lastWarn          = 0;

// ---- Test state ----
enum TestState {
  WAIT_SIGNAL,
  WAIT_SAFE_NEUTRAL,    // arm gate — all sticks centered, no fwd, no reverse intent
  WAIT_RUDDER_LEFT,
  WAIT_RUDDER_RIGHT,
  WAIT_FWD_DIFF,        // forward throttle + rudder offset → expect port ≠ stbd
  WAIT_REVERSE_PHASE_A, // right-stick down only — must NOT drive reverse (interlock test)
  WAIT_REVERSE_PHASE_B, // both sticks → reverse driven
  WAIT_FINAL_NEUTRAL,   // back to neutral before LIVE
  LIVE
};
TestState state = WAIT_SIGNAL;

bool failsafeActive       = false;
bool revPhaseAObserved    = false;   // we saw the operator try right-stick-down with left stick centered
unsigned long lastHint    = 0;
unsigned long phaseAStart = 0;
const unsigned long HINT_INTERVAL_MS    = 8000;
const unsigned long FREEZE_TIMEOUT_MS   = 500;
const unsigned long NO_FRAME_TIMEOUT_MS = 500;
const unsigned long PHASE_A_HOLD_MS     = 1500;  // operator must hold right-stick-down this long
                                                  // with left stick centered before phase A passes

// ---- Helpers ----
uint16_t usTicks(uint16_t microseconds) {
  return (uint16_t)((microseconds / 20000.0f) * 4096);
}

bool atNeutral(uint16_t stickUs) {
  int d = (int)stickUs - 1500;
  if (d < 0) d = -d;
  return d <= NEUTRAL_DEADBAND_US;
}

bool leftStickAllTheWayDown(uint16_t leftStickUs) {
  return leftStickUs <= LEFT_STICK_DOWN_US;
}

bool rightStickCommandingReverse(uint16_t rightStickVUs) {
  return rightStickVUs < (1500 - REVERSE_DEADBAND_US);
}

uint16_t clampRudder(uint16_t rawUs) {
  if (rawUs < 1000) rawUs = 1000;
  if (rawUs > 2000) rawUs = 2000;
  if (RUDDER_REVERSE) rawUs = 3000 - rawUs;
  if (rawUs < RUDDER_MIN_US) rawUs = RUDDER_MIN_US;
  if (rawUs > RUDDER_MAX_US) rawUs = RUDDER_MAX_US;
  return rawUs;
}

// Combine left stick (forward) + right stick V (reverse) + interlock into a
// single throttle µs in [MIN_REV_US..MAX_FWD_US], with 1500 = stop.
uint16_t computeThrottleUs(uint16_t leftStickUs, uint16_t rightStickVUs) {
  // Forward: left stick > 1500 + dead band → forward throttle, ignore reverse stick.
  if (leftStickUs > 1500 + NEUTRAL_DEADBAND_US) {
    uint16_t fwd = leftStickUs;
    if (fwd > MAX_FWD_US) fwd = MAX_FWD_US;
    if (fwd < 1500)       fwd = 1500;
    return fwd;
  }
  // Reverse: requires interlock — left stick all the way down AND right stick V below neutral.
  if (leftStickAllTheWayDown(leftStickUs) && rightStickCommandingReverse(rightStickVUs)) {
    uint16_t rev = rightStickVUs;
    if (rev < MIN_REV_US) rev = MIN_REV_US;
    if (rev > 1500)       rev = 1500;
    return rev;
  }
  // Anything else — including right-stick-down without the interlock — is neutral.
  return 1500;
}

// Differential-thrust mix from motors.cpp:46. Produces (port, stbd) µs pair.
void computePortStbd(uint16_t throttleUs, uint16_t rudderUs,
                     uint16_t &portUs, uint16_t &stbdUs) {
  float rudderDelta = (float)((int)rudderUs - 1500) / 500.0f;
  float diffUs = rudderDelta * DIFF_THRUST_FACTOR * ((int)throttleUs - 1500);
  int port = (int)throttleUs + (int)diffUs;
  int stbd = (int)throttleUs - (int)diffUs;
  // Clamp each side to its allowed range (matches what the ESC will see).
  if (port < MIN_REV_US) port = MIN_REV_US;
  if (port > MAX_FWD_US) port = MAX_FWD_US;
  if (stbd < MIN_REV_US) stbd = MIN_REV_US;
  if (stbd > MAX_FWD_US) stbd = MAX_FWD_US;
  portUs = (uint16_t)port;
  stbdUs = (uint16_t)stbd;
}

void writeOutputs(uint16_t portUs, uint16_t stbdUs, uint16_t rudderUs) {
  if (!pcaPresent) return;
  pca.setPWM(PORT_ESC_PCA_CHANNEL, 0, usTicks(portUs));
  pca.setPWM(STBD_ESC_PCA_CHANNEL, 0, usTicks(stbdUs));
  pca.setPWM(RUDDER_PCA_CHANNEL,   0, usTicks(rudderUs));
}

void writeNeutral() { writeOutputs(1500, 1500, 1500); }

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
  Serial.printf(">>> FAILSAFE — %s. All outputs neutral. <<<\n", reason);
  Serial.println("Re-center every stick once signal returns to re-arm.");
  if (state != WAIT_SIGNAL) state = WAIT_SAFE_NEUTRAL;
  // reset gate progress that depends on continuous signal
  revPhaseAObserved = false;
  phaseAStart = 0;
  lastHint = millis();
}

void clearFailsafe() {
  if (!failsafeActive) return;
  failsafeActive = false;
  Serial.println();
  Serial.println(">>> FAILSAFE RECOVERED — re-center sticks to re-arm. <<<");
  lastHint = millis();
}

bool allSticksAtSafeNeutral(uint16_t leftStick, uint16_t rightStickV, uint16_t rudderRaw) {
  // Forward throttle not commanded.
  if (leftStick > 1500 + NEUTRAL_DEADBAND_US) return false;
  // Right stick V near center (no reverse intent and no random offset).
  if (!atNeutral(rightStickV)) return false;
  // Rudder near center.
  if (!atNeutral(rudderRaw)) return false;
  return true;
}

void printHint(uint16_t leftStick, uint16_t rightStickV, uint16_t rudderRaw) {
  if (failsafeActive) return;
  switch (state) {
    case WAIT_SIGNAL:
      Serial.println("  ...waiting for iBUS frames. Turn on your transmitter.");
      break;
    case WAIT_SAFE_NEUTRAL:
      Serial.printf("  ...waiting for safe neutral.  L-stick=%u  R-stickV=%u  rudder=%u (need all near 1500, fwd not advanced).\n",
                    leftStick, rightStickV, rudderRaw);
      break;
    case WAIT_RUDDER_LEFT:
      Serial.printf("  ...sweep RUDDER LEFT (need ≤ %u µs). Currently %u.\n",
                    RUDDER_LEFT_THRESHOLD_US, rudderRaw);
      break;
    case WAIT_RUDDER_RIGHT:
      Serial.printf("  ...sweep RUDDER RIGHT (need ≥ %u µs). Currently %u.\n",
                    RUDDER_RIGHT_THRESHOLD_US, rudderRaw);
      break;
    case WAIT_FWD_DIFF:
      Serial.printf("  ...hold rudder OFF-CENTER (≥ %u µs from 1500), then advance FWD throttle past %u µs.\n",
                    RUDDER_OFFSET_FOR_DIFF_US, FWD_ADVANCE_THRESHOLD_US);
      break;
    case WAIT_REVERSE_PHASE_A:
      Serial.println("  ...REVERSE PHASE A: pull right stick DOWN with left stick centered.");
      Serial.println("     Motors must NOT reverse (interlock). Hold for ~1.5 s.");
      break;
    case WAIT_REVERSE_PHASE_B:
      Serial.println("  ...REVERSE PHASE B: left stick ALL THE WAY DOWN + right stick DOWN.");
      Serial.println("     Motors should now reverse.");
      break;
    case WAIT_FINAL_NEUTRAL:
      Serial.println("  ...return all sticks to neutral to enter LIVE mode.");
      break;
    case LIVE:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_17_manual_sailing");
  Serial.println("========================================");
  Serial.println("SAFETY: PROPS OFF or boat firmly secured.");
  Serial.printf("Rudder    stick: CH%d (idx %d)\n", RUDDER_CHANNEL_INDEX + 1, RUDDER_CHANNEL_INDEX);
  Serial.printf("Reverse   stick: CH%d (idx %d) — right-stick V, down = reverse\n",
                REVERSE_CHANNEL_INDEX + 1, REVERSE_CHANNEL_INDEX);
  Serial.printf("Throttle  stick: CH%d (idx %d) — left-stick V, fwd + reverse-enable\n",
                THROTTLE_CHANNEL_INDEX + 1, THROTTLE_CHANNEL_INDEX);
  Serial.printf("Rudder limits:   %u..%u µs (from test_15)\n", RUDDER_MIN_US, RUDDER_MAX_US);
  Serial.printf("Throttle limits: fwd cap %u µs, rev cap %u µs\n", MAX_FWD_US, MIN_REV_US);
  Serial.printf("Diff thrust:     %.2f (mirrors motors.cpp)\n", DIFF_THRUST_FACTOR);
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
    Serial.println("       PASS 1/5 — sticks confirmed at safe neutral.");
  } else {
    Serial.println("[FAIL] PCA9685 not found at 0x40. Check I2C wiring (SDA=21, SCL=22).");
    while (true) delay(1000);
  }

  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);

  Serial.println();
  Serial.println("Step 1: turn on TX. Center all sticks (left-stick V at 1500, right stick at rest).");
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

  uint16_t leftStick   = channels[THROTTLE_CHANNEL_INDEX];
  uint16_t rightStickV = channels[REVERSE_CHANNEL_INDEX];
  uint16_t rudderRaw   = channels[RUDDER_CHANNEL_INDEX];

  // ---- Compute outputs every frame so live values reflect current sticks ----
  uint16_t rudderUs   = clampRudder(rudderRaw);
  uint16_t throttleUs = computeThrottleUs(leftStick, rightStickV);
  uint16_t portUs, stbdUs;
  computePortStbd(throttleUs, rudderUs, portUs, stbdUs);

  // Force outputs to neutral whenever we're not authorized to drive.
  bool driveAuthorized = !failsafeActive &&
                         state != WAIT_SIGNAL &&
                         state != WAIT_SAFE_NEUTRAL;
  if (!driveAuthorized) {
    portUs = 1500;
    stbdUs = 1500;
    // Rudder stays at the commanded position even when motors are locked,
    // so the operator can sweep during the rudder gates without spinning props.
    // EXCEPT in WAIT_SIGNAL/failsafe — there we force rudder neutral too.
    if (state == WAIT_SIGNAL || failsafeActive) rudderUs = 1500;
  }

  writeOutputs(portUs, stbdUs, rudderUs);

  // ---- State machine ----
  switch (state) {
    case WAIT_SIGNAL:
      Serial.println();
      Serial.println(">>> SIGNAL ACQUIRED <<<");
      Serial.printf("  L-stick=%u  R-stickV=%u  rudder=%u\n", leftStick, rightStickV, rudderRaw);
      Serial.println("----------------------------------------");
      lastChannelChange = millis();
      state = WAIT_SAFE_NEUTRAL;
      lastHint = millis();
      break;

    case WAIT_SAFE_NEUTRAL:
      if (!failsafeActive && allSticksAtSafeNeutral(leftStick, rightStickV, rudderRaw)) {
        Serial.println();
        Serial.println("PASS (1/5): all sticks at safe neutral. ARMED.");
        Serial.println("Step 2: sweep RUDDER LEFT to verify steering.");
        Serial.println("----------------------------------------");
        state = WAIT_RUDDER_LEFT;
        lastHint = millis();
      }
      break;

    case WAIT_RUDDER_LEFT:
      if (rudderRaw <= RUDDER_LEFT_THRESHOLD_US) {
        Serial.println();
        Serial.printf("PASS (2/5): rudder swept LEFT (CH%d=%u µs, output=%u µs).\n",
                      RUDDER_CHANNEL_INDEX + 1, rudderRaw, rudderUs);
        Serial.println("Step 3: sweep RUDDER RIGHT.");
        Serial.println("----------------------------------------");
        state = WAIT_RUDDER_RIGHT;
        lastHint = millis();
      }
      break;

    case WAIT_RUDDER_RIGHT:
      if (rudderRaw >= RUDDER_RIGHT_THRESHOLD_US) {
        Serial.println();
        Serial.printf("PASS (3/5): rudder swept RIGHT (CH%d=%u µs, output=%u µs).\n",
                      RUDDER_CHANNEL_INDEX + 1, rudderRaw, rudderUs);
        Serial.println("Step 4: hold rudder OFF-CENTER (either direction) and advance FORWARD throttle.");
        Serial.println("        Both motors should spin, with PORT and STBD outputs splitting.");
        Serial.println("----------------------------------------");
        state = WAIT_FWD_DIFF;
        lastHint = millis();
      }
      break;

    case WAIT_FWD_DIFF: {
      int rudderOffset = (int)rudderRaw - 1500;
      if (rudderOffset < 0) rudderOffset = -rudderOffset;
      bool fwdAdvanced = leftStick >= FWD_ADVANCE_THRESHOLD_US;
      bool rudderOff   = rudderOffset >= (int)RUDDER_OFFSET_FOR_DIFF_US;
      int splitUs = (int)portUs - (int)stbdUs;
      if (splitUs < 0) splitUs = -splitUs;
      bool splitOK = splitUs >= (int)DIFF_MIN_SPLIT_US;
      if (fwdAdvanced && rudderOff && splitOK) {
        Serial.println();
        Serial.printf("PASS (4/5): differential thrust verified.\n");
        Serial.printf("            L-stick=%u  rudder=%u (Δ%+d)  PORT=%u  STBD=%u  |split|=%d µs\n",
                      leftStick, rudderRaw, (int)rudderRaw - 1500, portUs, stbdUs, splitUs);
        Serial.println("Step 5: return forward throttle to neutral, then we test REVERSE.");
        Serial.println("----------------------------------------");
        state = WAIT_REVERSE_PHASE_A;
        revPhaseAObserved = false;
        phaseAStart = 0;
        lastHint = millis();
      }
      break;
    }

    case WAIT_REVERSE_PHASE_A: {
      // Don't progress until forward throttle is back to neutral.
      bool fwdAtNeutral = !(leftStick > 1500 + NEUTRAL_DEADBAND_US);
      if (!fwdAtNeutral) break;

      // Phase A condition: right stick down, BUT left stick NOT all the way down (centered/idle).
      // The interlock should keep throttleUs at 1500.
      bool rightDown    = rightStickCommandingReverse(rightStickV);
      bool leftCentered = atNeutral(leftStick);
      if (rightDown && leftCentered) {
        if (phaseAStart == 0) phaseAStart = millis();
        if (throttleUs != 1500) {
          // This would mean the interlock is broken — abort gate.
          Serial.println();
          Serial.printf("[FAIL] interlock broken: right-stick down with left centered drove throttle to %u µs.\n",
                        throttleUs);
          Serial.println("       Halting. Check computeThrottleUs() logic.");
          state = WAIT_SAFE_NEUTRAL;
          phaseAStart = 0;
          break;
        }
        if (millis() - phaseAStart >= PHASE_A_HOLD_MS) {
          revPhaseAObserved = true;
          Serial.println();
          Serial.println("[OK] interlock holds: right-stick down with left centered → motors stay at 1500.");
          Serial.println("Step 5b: now push LEFT STICK ALL THE WAY DOWN + right stick down → motors should reverse.");
          state = WAIT_REVERSE_PHASE_B;
          lastHint = millis();
        }
      } else {
        // Reset hold timer if operator releases.
        phaseAStart = 0;
      }
      break;
    }

    case WAIT_REVERSE_PHASE_B: {
      bool leftDown  = leftStickAllTheWayDown(leftStick);
      bool rightDown = rightStickCommandingReverse(rightStickV);
      bool reversing = throttleUs < 1500 - NEUTRAL_DEADBAND_US;
      if (leftDown && rightDown && reversing) {
        Serial.println();
        Serial.printf("PASS (5/5): reverse engaged via interlock. L-stick=%u  R-stickV=%u  throttle out=%u µs  PORT=%u  STBD=%u\n",
                      leftStick, rightStickV, throttleUs, portUs, stbdUs);
        Serial.println("Return all sticks to neutral to finish.");
        Serial.println("----------------------------------------");
        state = WAIT_FINAL_NEUTRAL;
        lastHint = millis();
      }
      break;
    }

    case WAIT_FINAL_NEUTRAL:
      if (!failsafeActive && allSticksAtSafeNeutral(leftStick, rightStickV, rudderRaw)) {
        Serial.println();
        Serial.println("========================================");
        Serial.println("  ALL GATES PASSED — manual sailing verified");
        Serial.println("========================================");
        Serial.println("Sketch is now LIVE. Sticks drive port/stbd ESCs + rudder with");
        Serial.println("differential mixing and reverse interlock. Failsafe is active.");
        Serial.println("Cut TX power to verify auto-stop, then re-center to re-arm.");
        state = LIVE;
        lastHint = millis();
      }
      break;

    case LIVE:
      break;
  }
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

  // Secondary failsafe: no frames at all (RX disconnected).
  if (state != WAIT_SIGNAL && !failsafeActive &&
      millis() - lastFrame > NO_FRAME_TIMEOUT_MS) {
    enterFailsafe("no iBUS frames (RX disconnected?)");
  }

  // Periodic hint while waiting (uses last-seen channel values).
  if (state != LIVE && millis() - lastHint > HINT_INTERVAL_MS) {
    uint16_t leftStick   = prevValid ? prevChannels[THROTTLE_CHANNEL_INDEX] : 0;
    uint16_t rightStickV = prevValid ? prevChannels[REVERSE_CHANNEL_INDEX]  : 0;
    uint16_t rudderRaw   = prevValid ? prevChannels[RUDDER_CHANNEL_INDEX]   : 0;
    printHint(leftStick, rightStickV, rudderRaw);
    lastHint = millis();
  }
}
