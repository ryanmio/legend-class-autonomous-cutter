/*
 * test_01_servo_sweep.ino
 * Stage 1: ESP32 → PCA9685 → Rudder Servo
 * Expected: servo sweeps left → center → right → center, repeating
 * No battery needed. Powered by USB + 5V bench supply on PCA9685 V+.
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);

// Convert microseconds to PCA9685 ticks (50 Hz = 20,000 µs period)
uint16_t us(uint16_t microseconds) {
  return (uint16_t)((microseconds / 20000.0f) * 4096);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);       // SDA=21, SCL=22
  pca.begin();
  pca.setOscillatorFrequency(27000000); // Adjust if servo positions are off: try 25000000
  pca.setPWMFreq(50);
  Serial.println("Stage 1: Servo sweep starting on ch2");
}

void loop() {
  pca.setPWM(2, 0, us(1000)); Serial.println("LEFT");   delay(1000);
  pca.setPWM(2, 0, us(1500)); Serial.println("CENTER"); delay(500);
  pca.setPWM(2, 0, us(2000)); Serial.println("RIGHT");  delay(1000);
  pca.setPWM(2, 0, us(1500)); Serial.println("CENTER"); delay(500);
}
