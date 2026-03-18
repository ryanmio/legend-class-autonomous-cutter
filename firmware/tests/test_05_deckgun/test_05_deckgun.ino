/*
 * test_05_deckgun.ino
 * Bench test for deck gun pan and tilt servos — direct GPIO, no PCA9685.
 *
 *   GPIO 25 — Pan  (MG90S, rotates turret)
 *   GPIO 26 — Tilt (2.1 g micro servo, barrel pitch)
 *
 * Wiring:
 *   Servo red   → ESP32 5V/VIN pin  (USB 5V rail — do NOT use 3.3V)
 *   Servo brown → ESP32 GND
 *   Pan  signal → GPIO 25
 *   Tilt signal → GPIO 26
 *
 * Tilt limits are intentionally narrow (±100 µs) to protect the tiny
 * servo and pushrod linkage. Widen carefully once you know the end-stops.
 *
 * Open Serial Monitor at 115200 to watch the step log.
 *
 * Library required: ESP32Servo (install via Library Manager)
 */

#include <ESP32Servo.h>

// ── Pins ─────────────────────────────────────────────────────────────────────
#define PIN_GUN_PAN     25
#define PIN_GUN_TILT    26

// ── Pan servo limits (µs) ────────────────────────────────────────────────────
// MG90S: restrict to ±60° of centre until mechanical stops are confirmed.
#define PAN_CENTER_US   1500
#define PAN_LEFT_US     1100
#define PAN_RIGHT_US    1900

// ── Tilt servo limits (µs) ──────────────────────────────────────────────────
// 2.1 g micro servo — keep tight until linkage geometry is measured.
#define TILT_CENTER_US  1500
#define TILT_UP_US      1400   // Decrease = barrel up — flip if reversed
#define TILT_DOWN_US    1600   // Increase = barrel down

// ── Timing ──────────────────────────────────────────────────────────────────
#define STEP_HOLD_MS    800
#define STEP_PAUSE_MS   300

Servo panServo;
Servo tiltServo;

// ── Sequence table ───────────────────────────────────────────────────────────
struct StepDef {
  Servo*      servo;
  uint16_t    pulseUs;
  uint32_t    holdMs;
  const char* label;
};

StepDef STEPS[] = {
  { &panServo,  PAN_CENTER_US,  STEP_PAUSE_MS, "PAN  CENTER" },
  { &panServo,  PAN_LEFT_US,    STEP_HOLD_MS,  "PAN  LEFT"   },
  { &panServo,  PAN_CENTER_US,  STEP_PAUSE_MS, "PAN  CENTER" },
  { &panServo,  PAN_RIGHT_US,   STEP_HOLD_MS,  "PAN  RIGHT"  },
  { &panServo,  PAN_CENTER_US,  STEP_PAUSE_MS, "PAN  CENTER" },
  { &tiltServo, TILT_CENTER_US, STEP_PAUSE_MS, "TILT CENTER" },
  { &tiltServo, TILT_UP_US,     STEP_HOLD_MS,  "TILT UP"     },
  { &tiltServo, TILT_CENTER_US, STEP_PAUSE_MS, "TILT CENTER" },
  { &tiltServo, TILT_DOWN_US,   STEP_HOLD_MS,  "TILT DOWN"   },
  { &tiltServo, TILT_CENTER_US, STEP_PAUSE_MS, "TILT CENTER" },
};
const uint8_t NUM_STEPS = sizeof(STEPS) / sizeof(STEPS[0]);

uint8_t  currentStep = 0;
uint32_t stepStartMs = 0;

void setup() {
  Serial.begin(115200);

  panServo.attach(PIN_GUN_PAN,   PAN_LEFT_US,   PAN_RIGHT_US);
  tiltServo.attach(PIN_GUN_TILT, TILT_UP_US,    TILT_DOWN_US);

  panServo.writeMicroseconds(PAN_CENTER_US);
  tiltServo.writeMicroseconds(TILT_CENTER_US);

  Serial.println("=== test_05_deckgun ===");
  Serial.println("GPIO25=pan  GPIO26=tilt  (no PCA9685)");
  Serial.printf( "Pan  limits: LEFT=%d  CENTER=%d  RIGHT=%d µs\n",
                 PAN_LEFT_US, PAN_CENTER_US, PAN_RIGHT_US);
  Serial.printf( "Tilt limits: UP=%d  CENTER=%d  DOWN=%d µs\n",
                 TILT_UP_US, TILT_CENTER_US, TILT_DOWN_US);
  Serial.println("-- servos centred, starting sweep in 1 s --");
  Serial.println("servo  position      pulse");
  Serial.println("-----  -----------  ------");

  delay(1000);
  stepStartMs = millis();
}

void loop() {
  if (millis() - stepStartMs < STEPS[currentStep].holdMs) return;

  const StepDef& s = STEPS[currentStep];
  s.servo->writeMicroseconds(s.pulseUs);
  Serial.printf("%-5s  %-11s  %4d µs\n",
                (s.servo == &panServo) ? "pan" : "tilt",
                s.label, s.pulseUs);

  stepStartMs = millis();
  currentStep = (currentStep + 1) % NUM_STEPS;
}
