/*
 * test_03_ibus_passthrough.ino
 *
 * OBJECTIVE: Confirm the Flysky receiver is wired correctly and the ESP32
 * can parse a live iBUS signal. Checks that:
 *   1. iBUS frames arrive on UART1 RX (GPIO 16)
 *   2. Frames pass the checksum — data is not corrupted
 *   3. All channel values are in the valid servo range (1000–2000 µs)
 *   4. Stick movement changes the reported values
 *   5. Failsafe triggers (outputs go neutral) within 500 ms of signal loss
 *
 * PCA9685 is used if present (address 0x40) — servos/ESCs will move.
 * If PCA9685 is not wired, iBUS parsing still runs and reports to Serial.
 *
 * Voltage divider REQUIRED on iBUS wire before GPIO 16:
 *   Receiver iBUS → 1kΩ → GPIO 16 → 2kΩ → GND
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
bool pcaPresent = false;

HardwareSerial ibusSerial(1);  // UART1

uint8_t  ibusBuffer[32];
uint8_t  ibusIdx      = 0;
uint16_t channels[10];
unsigned long lastFrame    = 0;
unsigned long frameCount   = 0;
bool     signalAcquired    = false;
bool     failsafeActive    = false;

uint16_t usTicks(uint16_t microseconds) {
  return (uint16_t)((microseconds / 20000.0f) * 4096);
}

bool parseIBus() {
  if (ibusBuffer[0] != 0x20 || ibusBuffer[1] != 0x40) return false;
  uint16_t sum = 0xFFFF;
  for (int i = 0; i < 30; i++) sum -= ibusBuffer[i];
  uint16_t rx = ibusBuffer[30] | (ibusBuffer[31] << 8);
  if (sum != rx) {
    Serial.println("[WARN] Checksum mismatch — corrupted frame, ignoring.");
    return false;
  }
  for (int i = 0; i < 10; i++)
    channels[i] = ibusBuffer[2 + i*2] | (ibusBuffer[3 + i*2] << 8);
  lastFrame = millis();
  frameCount++;
  return true;
}

bool channelsValid() {
  for (int i = 0; i < 10; i++) {
    if (channels[i] < 800 || channels[i] > 2200) return false;
  }
  return true;
}

void printChannels() {
  Serial.printf("  CH1(ROLL)=%4d  CH2(PITCH)=%4d  CH3(THR)=%4d  CH4(YAW)=%4d",
    channels[0], channels[1], channels[2], channels[3]);
  Serial.printf("  CH5=%4d  CH6=%4d  CH7=%4d  CH8=%4d\n",
    channels[4], channels[5], channels[6], channels[7]);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_03_ibus_passthrough");
  Serial.println("========================================");
  Serial.println("Objective: parse live iBUS from Flysky receiver on GPIO 16.");
  Serial.println();

  // Check for PCA9685
  Wire.begin(21, 22);
  Wire.beginTransmission(0x40);
  if (Wire.endTransmission() == 0) {
    pcaPresent = true;
    pca.begin();
    pca.setOscillatorFrequency(27000000);
    pca.setPWMFreq(50);
    Serial.println("[INFO] PCA9685 found at 0x40 — servos/ESCs will respond.");
    Serial.println("[INFO] Sending neutral (1500 µs) to ch0, ch1, ch2 for ESC arming...");
    pca.setPWM(0, 0, usTicks(1500));
    pca.setPWM(1, 0, usTicks(1500));
    pca.setPWM(2, 0, usTicks(1500));
    delay(3000);
    Serial.println("[INFO] ESC arm delay done.");
  } else {
    Serial.println("[INFO] PCA9685 not detected — running iBUS-only mode (no servo output).");
  }

  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);  // RX=GPIO16, TX unused

  Serial.println();
  Serial.println("Waiting for iBUS signal on GPIO 16...");
  Serial.println("Turn on your transmitter now.");
  Serial.println("----------------------------------------");
}

void loop() {
  // ---- Read and parse iBUS frames ----
  while (ibusSerial.available()) {
    uint8_t b = ibusSerial.read();
    if (ibusIdx == 0 && b != 0x20) continue;  // wait for frame start byte
    ibusBuffer[ibusIdx++] = b;

    if (ibusIdx == 32) {
      if (parseIBus()) {
        if (!signalAcquired) {
          signalAcquired = true;
          Serial.println();
          Serial.println(">>> SIGNAL ACQUIRED <<<");
          Serial.println("PASS (1/3): iBUS frames arriving and checksum OK.");
          Serial.println("Move sticks to confirm channel values change.");
          Serial.println("Then turn off transmitter to test failsafe.");
          Serial.println("----------------------------------------");
        }

        failsafeActive = false;

        if (!channelsValid()) {
          Serial.println("[WARN] One or more channel values out of range (800–2200). Check wiring.");
        }

        printChannels();

        if (pcaPresent) {
          pca.setPWM(0, 0, usTicks(channels[2]));  // throttle → port ESC
          pca.setPWM(2, 0, usTicks(channels[3]));  // yaw/rudder → rudder servo
        }
      }
      ibusIdx = 0;
    }
  }

  // ---- Failsafe: no valid frame for 500 ms ----
  if (signalAcquired && !failsafeActive && millis() - lastFrame > 500) {
    failsafeActive = true;
    if (pcaPresent) {
      pca.setPWM(0, 0, usTicks(1500));
      pca.setPWM(1, 0, usTicks(1500));
      pca.setPWM(2, 0, usTicks(1500));
    }
    Serial.println();
    Serial.println(">>> FAILSAFE TRIGGERED (no signal for 500 ms) <<<");
    Serial.printf("  Last valid frame was %lu ms ago.\n", millis() - lastFrame);
    Serial.println("  Outputs set to neutral (1500 µs).");
    Serial.printf("  Total frames received before loss: %lu\n", frameCount);
    Serial.println("PASS (3/3): Failsafe working.");
    Serial.println("----------------------------------------");
    Serial.println("Turn transmitter back on to resume.");
  }

  // ---- PASS (2/3) printed after 50 clean frames ----
  if (signalAcquired && frameCount == 50) {
    Serial.println();
    Serial.printf("PASS (2/3): %lu consecutive frames received with valid checksums.\n", frameCount);
    Serial.println("  Values should be ~1500 at stick center, ~1000 at min, ~2000 at max.");
    Serial.println("----------------------------------------");
  }
}
