/*
 * test_02_esc_motor.ino
 * Stage 2: ESP32 → PCA9685 → ESC → Motor
 * Arms both ESCs, then ramps port motor (ch0) up and back.
 * Starboard (ch1) stays at neutral the whole time.
 * PROPS OFF or motor unloaded during this test.
 * Run on battery power (switch ON → ESCs get 14.8V).
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);

uint16_t us(uint16_t microseconds) {
  return (uint16_t)((microseconds / 20000.0f) * 4096);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  pca.begin();
  pca.setOscillatorFrequency(27000000);
  pca.setPWMFreq(50);

  // Arm both ESCs: send neutral for 3 seconds
  Serial.println("Arming ESCs (3 sec)...");
  pca.setPWM(0, 0, us(1500));
  pca.setPWM(1, 0, us(1500));
  delay(3000);
  Serial.println("Armed. Starting motor ramp.");
}

void loop() {
  // Ramp port motor 0 from neutral → 60% forward → back to neutral
  for (int pulse = 1500; pulse <= 1800; pulse += 10) {
    pca.setPWM(0, 0, us(pulse));
    Serial.print("Throttle: "); Serial.println(pulse);
    delay(100);
  }
  delay(1000);
  for (int pulse = 1800; pulse >= 1500; pulse -= 10) {
    pca.setPWM(0, 0, us(pulse));
    delay(100);
  }
  pca.setPWM(0, 0, us(1500));
  Serial.println("Back to neutral. Waiting 3 sec.");
  delay(3000);
}
