/*
 * test_05_deckgun.ino
 * Bench test for deck gun pan servo (continuous rotation).
 * Direct GPIO — no PCA9685.
 *
 *   GPIO 25 — Pan (continuous rotation servo)
 *
 * Tilt was dropped — see NOTES.md.
 *
 * Wiring:
 *   Servo red   → ESP32 5V/VIN pin  (USB 5V rail — do NOT use 3.3V)
 *   Servo brown → ESP32 GND
 *   Pan  signal → GPIO 25
 *
 * Continuous servo: 1500 µs = stop, <1500 = right, >1500 = left.
 * Dead-band varies by servo — if it won't move, nudge values away from 1500.
 * If it creeps at stop, trim PAN_STOP_US ±5 µs until it holds still.
 *
 * Open Serial Monitor at 115200.
 *
 * Library required: ESP32Servo (install via Library Manager)
 */

#include <ESP32Servo.h>

#define PIN_GUN_PAN         25

#define PAN_STOP_US         1500
#define PAN_SLOW_RIGHT_US   1470   // nudge toward 1440 if too slow, toward 1500 if too fast
#define PAN_SLOW_LEFT_US    1560

#define PAN_TRAVEL_MS       600    // how long to rotate each direction
#define PAN_STOP_MS         2500   // dwell at stop between directions

Servo panServo;

enum PanState { PAN_RIGHT, PAN_STOPPING_1, PAN_LEFT, PAN_STOPPING_2 };
PanState panState   = PAN_STOPPING_2;
uint32_t panStateMs = 0;

void setup() {
  Serial.begin(115200);

  panServo.attach(PIN_GUN_PAN, 1000, 2000);
  panServo.writeMicroseconds(PAN_STOP_US);

  Serial.println("=== test_05_deckgun ===");
  Serial.printf("Pan: STOP=%d  RIGHT=%d  LEFT=%d µs  travel=%dms  stop=%dms\n",
                PAN_STOP_US, PAN_SLOW_RIGHT_US, PAN_SLOW_LEFT_US,
                PAN_TRAVEL_MS, PAN_STOP_MS);
  Serial.println("-- starting in 1 s --");

  delay(1000);
  panStateMs = millis();
}

void loop() {
  uint32_t now = millis();

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
}
