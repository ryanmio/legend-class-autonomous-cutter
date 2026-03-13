/*
 * test_03_ibus_passthrough.ino
 * Stage 3: Flysky receiver → ESP32 iBUS → PCA9685 → Servo + ESC
 * Right stick vertical (ch2 iBUS) → port ESC (PCA9685 ch0)
 * Right stick horizontal (ch3 iBUS) → rudder servo (PCA9685 ch2)
 * Serial monitor shows live channel values.
 * Voltage divider required on iBUS wire before GPIO 16.
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);

HardwareSerial ibusSerial(1);  // UART1

uint8_t  ibusBuffer[32];
uint8_t  ibusIdx   = 0;
uint16_t channels[10];
unsigned long lastFrame = 0;

uint16_t us(uint16_t microseconds) {
  return (uint16_t)((microseconds / 20000.0f) * 4096);
}

bool parseIBus() {
  if (ibusBuffer[0] != 0x20 || ibusBuffer[1] != 0x40) return false;
  uint16_t sum = 0xFFFF;
  for (int i = 0; i < 30; i++) sum -= ibusBuffer[i];
  uint16_t rx = ibusBuffer[30] | (ibusBuffer[31] << 8);
  if (sum != rx) return false;
  for (int i = 0; i < 10; i++)
    channels[i] = ibusBuffer[2 + i*2] | (ibusBuffer[3 + i*2] << 8);
  lastFrame = millis();
  return true;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  pca.begin();
  pca.setOscillatorFrequency(27000000);
  pca.setPWMFreq(50);

  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);  // RX=16 only

  // Arm ESCs
  Serial.println("Arming ESCs (3 sec)...");
  pca.setPWM(0, 0, us(1500));
  pca.setPWM(1, 0, us(1500));
  delay(3000);
  Serial.println("Ready. Move sticks.");
}

void loop() {
  while (ibusSerial.available()) {
    uint8_t b = ibusSerial.read();
    if (ibusIdx == 0 && b != 0x20) continue;
    ibusBuffer[ibusIdx++] = b;
    if (ibusIdx == 32) {
      if (parseIBus()) {
        uint16_t throttle = channels[2];  // Right stick vertical
        uint16_t rudder   = channels[3];  // Right stick horizontal
        pca.setPWM(0, 0, us(throttle));   // Port ESC
        pca.setPWM(2, 0, us(rudder));     // Rudder servo
        Serial.printf("THR=%d  RUD=%d\n", throttle, rudder);
      }
      ibusIdx = 0;
    }
  }

  // Failsafe: no frame for 500ms → neutral
  if (millis() - lastFrame > 500 && lastFrame > 0) {
    pca.setPWM(0, 0, us(1500));
    pca.setPWM(2, 0, us(1500));
    Serial.println("FAILSAFE");
  }
}
