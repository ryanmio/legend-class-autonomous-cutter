/*
 * test_13_ina219_battery.ino
 *
 * Bench/boat test. Confirms the INA219 current/voltage sensor is alive
 * on I2C at 0x41 and reads sensible values for a 4S LiPo battery.
 *
 * Pass gates:
 *   PASS 1/3  INA219 detected on I2C at 0x41
 *   PASS 2/3  Bus voltage in 4S LiPo range (10.0-17.5 V)
 *   PASS 3/3  Current reading is finite (not NaN, not stuck at rail)
 *
 * After PASS 3/3 the sketch streams a 1 Hz status line with bus
 * voltage, shunt voltage, current, power, and an estimated state of
 * charge for a resting 4S LiPo (3.0-4.2 V/cell → 0-100%).
 *
 * Wiring:
 *   INA219 VCC  → ESP32 3.3V (or 5V — INA219 is tolerant)
 *   INA219 GND  → ESP32 GND
 *   INA219 SDA  → ESP32 GPIO 21
 *   INA219 SCL  → ESP32 GPIO 22
 *   INA219 VIN+ → battery (+) terminal               (high side)
 *   INA219 VIN- → ESC / load (+)                     (after the shunt)
 *
 * Address: A0 jumper bridged → 0x41 (matches main firmware config).
 *
 * Library: install "Adafruit INA219" by Adafruit from Library Manager.
 *
 * Calibration: setCalibration_32V_2A() — 32 V bus range, ±3.2 A current
 * range with the breakout's stock 0.1 Ω shunt. This covers the 4S LiPo
 * voltage (max 16.8 V) and idle/no-load current. For motor draw above
 * ~3 A you'll see the current reading saturate; that's OK for a battery
 * monitor test (the voltage reading is what we care about) but the full
 * boat firmware needs a heftier shunt or an INA226/228 for runtime
 * current logging.
 */

#include <Wire.h>
#include <Adafruit_INA219.h>

const uint8_t  INA219_ADDR = 0x41;
const uint8_t  I2C_SDA_PIN = 21;
const uint8_t  I2C_SCL_PIN = 22;

// 4S LiPo voltage thresholds (resting / unloaded — under load these sag).
const float    LIPO_FULL_V   = 16.80f;   // 4.20 V/cell × 4
const float    LIPO_NOMINAL_V = 14.80f;  // 3.70 V/cell × 4 (storage / nominal)
const float    LIPO_ALARM_V  = 13.60f;   // matches BATTERY_ALARM_VOLTAGE
const float    LIPO_RTH_V    = 13.00f;   // matches BATTERY_RTH_VOLTAGE
const float    LIPO_DEAD_V   = 12.00f;   // 3.00 V/cell × 4 — damage range

// Sketch limits for the voltage sanity gate (PASS 2/3).
const float    PASS_VOLT_MIN = 10.0f;
const float    PASS_VOLT_MAX = 17.5f;

const unsigned long PRINT_INTERVAL_MS = 1000;

Adafruit_INA219 ina219(INA219_ADDR);

enum TestState { WAIT_DEVICE, WAIT_VOLTAGE, WAIT_CURRENT, LIVE };
TestState state = WAIT_DEVICE;

unsigned long bootMs      = 0;
unsigned long lastPrintMs = 0;

// Resting-cell SoC estimate. Linear segments between common LiPo points.
// Loaded packs read low; this is best taken with the boat idle.
float estimateSoCPercent(float vCell) {
  struct Pt { float v; float pct; };
  static const Pt lut[] = {
    {4.20f, 100.0f},
    {4.10f,  90.0f},
    {4.00f,  80.0f},
    {3.90f,  70.0f},
    {3.80f,  60.0f},
    {3.75f,  50.0f},
    {3.70f,  40.0f},
    {3.65f,  30.0f},
    {3.60f,  20.0f},
    {3.50f,  10.0f},
    {3.30f,   0.0f},
  };
  const int N = sizeof(lut) / sizeof(lut[0]);
  if (vCell >= lut[0].v)     return 100.0f;
  if (vCell <= lut[N-1].v)   return 0.0f;
  for (int i = 0; i < N - 1; i++) {
    if (vCell <= lut[i].v && vCell >= lut[i+1].v) {
      float t = (vCell - lut[i+1].v) / (lut[i].v - lut[i+1].v);
      return lut[i+1].pct + t * (lut[i].pct - lut[i+1].pct);
    }
  }
  return 0.0f;
}

const char* lipoState(float vPack) {
  if (vPack >= LIPO_FULL_V - 0.05f) return "FULL";
  if (vPack >= LIPO_NOMINAL_V)      return "good";
  if (vPack >= LIPO_ALARM_V)        return "ok";
  if (vPack >= LIPO_RTH_V)          return "ALARM (warn)";
  if (vPack >= LIPO_DEAD_V)         return "RTH (land now)";
  return "DEAD (damaged?)";
}

bool i2cDevicePresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void scanI2C() {
  Serial.printf("--- I2C bus scan (SDA=GPIO%d, SCL=GPIO%d) ---\n",
                I2C_SDA_PIN, I2C_SCL_PIN);
  int count = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (i2cDevicePresent(a)) {
      Serial.printf("  0x%02X", a);
      if (a == INA219_ADDR) Serial.print("  <- INA219 (this test)");
      else if (a == 0x40)   Serial.print("  <- PCA9685 (servo driver)");
      else if (a == 0x68)   Serial.print("  <- ICM-20948 (IMU)");
      else if (a == 0x70)   Serial.print("  <- PCA9685 all-call");
      Serial.println();
      count++;
    }
  }
  if (count == 0) Serial.println("  (no devices responded)");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  bootMs = millis();

  Serial.println("========================================");
  Serial.println("  test_13_ina219_battery");
  Serial.println("========================================");
  Serial.printf("Target: INA219 @ 0x%02X (SDA=GPIO%d, SCL=GPIO%d)\n",
                INA219_ADDR, I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.println("Battery: 4S LiPo (16.8 V full, 14.8 V nominal, 13.0 V RTH)");
  Serial.println();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  scanI2C();

  if (!i2cDevicePresent(INA219_ADDR)) {
    Serial.printf("FAIL (1/3): no I2C device at 0x%02X.\n", INA219_ADDR);
    Serial.println("  Check, in this order:");
    Serial.println("    1. INA219 VCC = 3.3 V or 5 V (multimeter at the breakout pad)");
    Serial.println("    2. GND tied between ESP32 and INA219");
    Serial.printf( "    3. SDA → GPIO%d, SCL → GPIO%d (4.7-10 kΩ pull-ups present)\n",
                   I2C_SDA_PIN, I2C_SCL_PIN);
    Serial.println("    4. A0 address jumper bridged (this test expects 0x41, not 0x40)");
    Serial.println("  The sketch will sit here. Fix wiring and reset.");
    return;
  }

  if (!ina219.begin()) {
    Serial.println("FAIL (1/3): I2C found 0x41 but ina219.begin() rejected it.");
    Serial.println("  Library installed but device may be a different INA chip.");
    return;
  }
  Serial.printf("PASS (1/3): INA219 detected and initialized (after %lu ms).\n",
                millis() - bootMs);

  // 32 V / 2 A range — covers 4S LiPo voltage; current saturates above ~3 A.
  ina219.setCalibration_32V_2A();
  state = WAIT_VOLTAGE;
  Serial.println("Step 2: confirming bus voltage in 4S LiPo range (10.0-17.5 V)...");
  Serial.println("----------------------------------------");
}

void printReading() {
  float busV    = ina219.getBusVoltage_V();
  float shuntmV = ina219.getShuntVoltage_mV();
  float currA   = ina219.getCurrent_mA() / 1000.0f;
  float vPack   = busV + (shuntmV / 1000.0f);   // bus is measured at V- (after shunt); add the drop back
  float vCell   = vPack / 4.0f;
  float soc     = estimateSoCPercent(vCell);
  float pwr     = vPack * currA;

  Serial.printf("V_bus=%5.2fV  V_shunt=%+6.2fmV  V_pack=%5.2fV  V/cell=%4.2fV  "
                "I=%+6.2fA  P=%+6.2fW  SoC=%3.0f%%  [%s]\n",
                busV, shuntmV, vPack, vCell, currA, pwr, soc, lipoState(vPack));
}

void loop() {
  if (state == WAIT_DEVICE) {
    delay(1000);
    return;
  }

  if (millis() - lastPrintMs < PRINT_INTERVAL_MS) return;
  lastPrintMs = millis();

  float busV    = ina219.getBusVoltage_V();
  float shuntmV = ina219.getShuntVoltage_mV();
  float currA   = ina219.getCurrent_mA() / 1000.0f;
  float vPack   = busV + (shuntmV / 1000.0f);

  switch (state) {
    case WAIT_VOLTAGE:
      if (vPack >= PASS_VOLT_MIN && vPack <= PASS_VOLT_MAX) {
        Serial.printf("PASS (2/3): pack voltage %.2f V is in 4S LiPo range.\n", vPack);
        Serial.println("Step 3: confirming current reading is finite...");
        Serial.println("----------------------------------------");
        state = WAIT_CURRENT;
      } else {
        Serial.printf("  V_pack=%.2f V — out of range (expected %.1f-%.1f). "
                      "Battery connected to VIN+? VIN- to load?\n",
                      vPack, PASS_VOLT_MIN, PASS_VOLT_MAX);
      }
      break;

    case WAIT_CURRENT:
      if (!isnan(currA) && currA > -10.0f && currA < 10.0f) {
        Serial.printf("PASS (3/3): current %.3f A reads cleanly.\n", currA);
        Serial.println();
        Serial.println("========================================");
        Serial.println("  ALL TESTS PASSED");
        Serial.println("========================================");
        Serial.println("Streaming 1 Hz battery telemetry. Ctrl-C / reset to stop.");
        state = LIVE;
      } else {
        Serial.printf("  current=%.3f A — looks invalid. Shunt wire off?\n", currA);
      }
      break;

    case LIVE:
      printReading();
      break;

    default:
      break;
  }
}
