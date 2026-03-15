/*
 * test_04_sonar.ino
 * Bench test for JSN-SR04T V3 ultrasonic depth sonar.
 * Pins: TRIG=27, ECHO=14 (matches config.h).
 *
 * Wiring:
 *   JSN-SR04T VCC  → 5V
 *   JSN-SR04T GND  → GND
 *   JSN-SR04T TRIG → GPIO 27
 *   JSN-SR04T ECHO → GPIO 14
 *   (ECHO is 5V tolerant on these pins — verify your ESP32 board before use)
 *
 * Open Serial Monitor at 115200. Point the transducer at a flat surface
 * (wall, floor, bucket of water) and watch the distance readings.
 *
 * Speed of sound constant is set for FRESHWATER (~13.4 µs/cm).
 * For air bench testing use AIR_SPEED_US_CM (58.0) instead so the
 * numbers make sense — there's a #define below to switch modes.
 */

#define TRIG_PIN          27
#define ECHO_PIN          14

// Swap these two lines to toggle between air and water calibration:
#define SOUND_SPEED_US_CM  58.0f   // Air — use this for bench testing in air
// #define SOUND_SPEED_US_CM  13.4f   // Freshwater — use this when deployed

#define ECHO_TIMEOUT_US   30000    // ~5 m in air / ~10 m in water
#define PING_INTERVAL_MS  200

unsigned long lastPingMs = 0;

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT_PULLDOWN);
  digitalWrite(TRIG_PIN, LOW);

  Serial.println("=== test_04_sonar ===");
  Serial.println("JSN-SR04T bench test — reading every 200 ms");
  Serial.printf("Sound speed constant: %.1f us/cm (%s)\n",
                SOUND_SPEED_US_CM,
                (SOUND_SPEED_US_CM < 20.0f) ? "WATER" : "AIR");
  Serial.println("--------------------");
}

float ping() {
  // 10 µs trigger pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if (duration == 0) return -1.0f;  // timeout / no echo

  return ((float)duration / 2.0f) / SOUND_SPEED_US_CM / 100.0f;  // metres
}

void loop() {
  if (millis() - lastPingMs < PING_INTERVAL_MS) return;
  lastPingMs = millis();

  // Raw debug: show ECHO pin state before trigger, then raw pulse duration
  Serial.printf("ECHO idle=%d  ", digitalRead(ECHO_PIN));

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long dur = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  Serial.printf("raw_us=%ld  ", dur);

  if (dur == 0) {
    Serial.println("NO ECHO");
  } else {
    float distM = ((float)dur / 2.0f) / SOUND_SPEED_US_CM / 100.0f;
    Serial.printf("dist=%.3f m (%.1f cm)\n", distM, distM * 100.0f);
  }
}
