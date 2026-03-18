/*
 * test_05_deckgun.ino
 * Bench test for deck gun pan (continuous rotation) and tilt (positional) servos.
 * Direct GPIO — no PCA9685.
 *
 *   GPIO 25 — Pan  (continuous rotation servo)
 *   GPIO 26 — Tilt (2.1 g micro positional servo)
 *
 * Wiring:
 *   Servo red   → ESP32 5V/VIN pin  (USB 5V rail — do NOT use 3.3V)
 *   Servo brown → ESP32 GND
 *   Pan  signal → GPIO 25
 *   Tilt signal → GPIO 26
 *
 * Continuous servo neutral (stop) = 1500 µs.
 * Pan sequence: slow right → stop → slow left → stop → repeat.
 * Tilt sequence fires once per pan cycle as a sanity check.
 *
 * Tune PAN_SLOW_RIGHT_US / PAN_SLOW_LEFT_US — the closer to 1500,
 * the slower the rotation. Dead-band varies by servo; trim if it
 * still creeps at "stop".
 *
 * Open Serial Monitor at 115200.
 *
 * Library required: ESP32Servo (install via Library Manager)
 */

#include <ESP32Servo.h>

// ── Pins ─────────────────────────────────────────────────────────────────────
#define PIN_GUN_PAN     25
#define PIN_GUN_TILT    26

// ── Continuous pan servo (µs) ────────────────────────────────────────────────
// 1500 = stop. Adjust the slow values if the servo creeps or won't move.
#define PAN_STOP_US         1500
#define PAN_SLOW_RIGHT_US   1470   // right — nudge toward 1440 if too slow, toward 1500 if too fast
#define PAN_SLOW_LEFT_US    1560   // left  — was labeled right before

// How long to rotate each direction before stopping (ms)
#define PAN_TRAVEL_MS       600
// How long to pause at stop between directions (ms)
#define PAN_STOP_MS         2500

// ── Positional tilt servo (µs) ───────────────────────────────────────────────
#define TILT_CENTER_US  1500
#define TILT_UP_US      1400
#define TILT_DOWN_US    1600
#define TILT_HOLD_MS    600

Servo panServo;
Servo tiltServo;

// ── Pan state machine ────────────────────────────────────────────────────────
enum PanState { PAN_RIGHT, PAN_STOPPING_1, PAN_LEFT, PAN_STOPPING_2 };
PanState panState    = PAN_STOPPING_2;   // start from stop
uint32_t panStateMs  = 0;

// ── Tilt fires once per full pan cycle ───────────────────────────────────────
enum TiltState { TILT_IDLE, TILT_GOING_UP, TILT_GOING_DOWN, TILT_RETURNING };
TiltState tiltState  = TILT_IDLE;
uint32_t  tiltStateMs = 0;

void startTiltCycle() {
  tiltState   = TILT_GOING_UP;
  tiltStateMs = millis();
  tiltServo.writeMicroseconds(TILT_UP_US);
  Serial.println("  tilt UP");
}

void setup() {
  Serial.begin(115200);

  panServo.attach(PIN_GUN_PAN,   1000, 2000);
  tiltServo.attach(PIN_GUN_TILT, 1000, 2000);

  panServo.writeMicroseconds(PAN_STOP_US);
  tiltServo.writeMicroseconds(TILT_CENTER_US);

  Serial.println("=== test_05_deckgun ===");
  Serial.println("pan=continuous  tilt=positional  (no PCA9685)");
  Serial.printf("Pan: STOP=%d  RIGHT=%d  LEFT=%d µs  travel=%dms\n",
                PAN_STOP_US, PAN_SLOW_RIGHT_US, PAN_SLOW_LEFT_US, PAN_TRAVEL_MS);
  Serial.printf("Tilt: UP=%d  CENTER=%d  DOWN=%d µs\n",
                TILT_UP_US, TILT_CENTER_US, TILT_DOWN_US);
  Serial.println("-- starting in 1 s --");

  delay(1000);
  panStateMs = millis();
}

void loop() {
  uint32_t now = millis();

  // ── Pan state machine ──────────────────────────────────────────────────────
  switch (panState) {
    case PAN_STOPPING_2:
      if (now - panStateMs >= PAN_STOP_MS) {
        panState   = PAN_RIGHT;
        panStateMs = now;
        panServo.writeMicroseconds(PAN_SLOW_RIGHT_US);
        Serial.println("starting right pan");
      }
      break;

    case PAN_RIGHT:
      if (now - panStateMs >= PAN_TRAVEL_MS) {
        panState   = PAN_STOPPING_1;
        panStateMs = now;
        panServo.writeMicroseconds(PAN_STOP_US);
        Serial.println("stop");
      }
      break;

    case PAN_STOPPING_1:
      if (now - panStateMs >= PAN_STOP_MS) {
        panState   = PAN_LEFT;
        panStateMs = now;
        panServo.writeMicroseconds(PAN_SLOW_LEFT_US);
        Serial.println("starting left pan");
      }
      break;

    case PAN_LEFT:
      if (now - panStateMs >= PAN_TRAVEL_MS) {
        panState   = PAN_STOPPING_2;
        panStateMs = now;
        panServo.writeMicroseconds(PAN_STOP_US);
        Serial.println("stop");
      }
      break;
  }

  // ── Tilt state machine ─────────────────────────────────────────────────────
  switch (tiltState) {
    case TILT_IDLE: break;

    case TILT_GOING_UP:
      if (now - tiltStateMs >= TILT_HOLD_MS) {
        tiltState   = TILT_GOING_DOWN;
        tiltStateMs = now;
        tiltServo.writeMicroseconds(TILT_DOWN_US);
        Serial.println("  tilt DOWN");
      }
      break;

    case TILT_GOING_DOWN:
      if (now - tiltStateMs >= TILT_HOLD_MS) {
        tiltState   = TILT_RETURNING;
        tiltStateMs = now;
        tiltServo.writeMicroseconds(TILT_CENTER_US);
        Serial.println("  tilt CENTER");
      }
      break;

    case TILT_RETURNING:
      if (now - tiltStateMs >= TILT_HOLD_MS) {
        tiltState = TILT_IDLE;
      }
      break;
  }
}
