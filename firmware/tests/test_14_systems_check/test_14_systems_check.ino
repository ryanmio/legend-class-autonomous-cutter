/*
 * test_14_systems_check.ino
 *
 * Post-waterproofing bringup. Verifies every critical subsystem is wired
 * correctly and returning sensible values, WITHOUT moving any servo, ESC,
 * or other actuator. Safe to run on a fully assembled boat sitting on the
 * bench while you reconnect things one by one.
 *
 * Pinout matches the boat (legend_cutter/config.h), NOT the bench tests.
 *
 * Pass gates (in order):
 *   PASS 1/6  I2C bus scan finds all four expected addresses
 *               0x40  PCA9685   (servo driver)
 *               0x41  INA219    (battery monitor)
 *               0x68  ICM-20948 (IMU)
 *               0x70  PCA9685 all-call broadcast (informational, not gated)
 *   PASS 2/6  PCA9685 accepts setPWMFreq(50) without I2C error
 *   PASS 3/6  INA219 reports a 4S-LiPo-plausible bus voltage (10.0-17.5 V)
 *   PASS 4/6  ICM-20948 begin() succeeds and accel magnitude ≈ 1 g
 *   PASS 5/6  GPS produces a valid NMEA sentence within 5 s
 *               (NO fix required — indoor reassembly is fine)
 *   PASS 6/6  DF1201S DFPlayer Pro responds to begin() within 5 s
 *
 * After all six gates, streams a 1 Hz "all systems" line so you can watch
 * for dropouts as you wiggle harnesses, close the hatch, etc.
 *
 * Wiring (boat config — see legend_cutter/config.h):
 *   I2C SDA           → GPIO 21          (PCA9685, INA219, ICM-20948 share)
 *   I2C SCL           → GPIO 22
 *   GPS  TX  (white)  → GPIO 17          (UART2 RX)   *
 *   GPS  RX  (green)  → GPIO 4           (UART2 TX, optional)   *
 *   DF1201S TX        → GPIO 25          (UART1 RX in this sketch)
 *   DF1201S RX        → GPIO 26          (UART1 TX in this sketch)
 *
 *   * This batch of BN-220s has white=TX / green=RX, REVERSED from the
 *     typical convention. Confirmed 2026-05-03 — see test_10/10b NOTES.
 *
 * Libraries:
 *   Adafruit PWM Servo Driver Library     (PCA9685)
 *   Adafruit INA219                       (INA219)
 *   SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library
 *   TinyGPSPlus  by Mikal Hart            (GPS)
 *   DFRobot_DF1201S  by DFRobot           (audio)
 *
 * Notes:
 *   - This sketch never calls setPWM() on the PCA9685, never arms an ESC,
 *     never moves a servo. PCA9685 outputs stay at whatever the chip's
 *     power-on default is until something else commands them.
 *   - The DFPlayer test does NOT play audio — only confirms the module
 *     ACKs over UART. Use test_11_dfplayer for audio verification.
 *   - GPS uses UART2 and DFPlayer uses UART1 (different from test_11
 *     which uses UART2). They cannot share — keep them separate.
 */

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_INA219.h>
#include "ICM_20948.h"
#include <TinyGPSPlus.h>
#include <DFRobot_DF1201S.h>

// ---------- Pin / address constants (match config.h) ----------
const uint8_t  I2C_SDA_PIN  = 21;
const uint8_t  I2C_SCL_PIN  = 22;
const uint32_t I2C_FREQ     = 400000;

const uint8_t  PCA_ADDR     = 0x40;
const uint8_t  INA_ADDR     = 0x41;
const uint8_t  IMU_ADDR     = 0x68;
const uint8_t  PCA_ALLCALL  = 0x70;

const uint8_t  GPS_RX_PIN   = 17;   // ESP32 reads (← BN-220 TX, white)
const uint8_t  GPS_TX_PIN   = 4;    // ESP32 writes (→ BN-220 RX, green)
const uint32_t GPS_BAUD     = 9600;

const uint8_t  DFP_RX_PIN   = 25;   // ESP32 reads (← DF1201S TX)
const uint8_t  DFP_TX_PIN   = 26;   // ESP32 writes (→ DF1201S RX)
const uint32_t DFP_BAUD     = 115200;

// ---------- Sanity ranges ----------
const float    LIPO_V_MIN   = 10.0f;
const float    LIPO_V_MAX   = 17.5f;
const float    ACCEL_MIN_MG = 700.0f;   // generous: ~0.7 g
const float    ACCEL_MAX_MG = 1300.0f;  // generous: ~1.3 g

const unsigned long GPS_NMEA_TIMEOUT_MS = 5000;
const unsigned long DFP_BEGIN_TIMEOUT_MS = 5000;
const unsigned long PRINT_INTERVAL_MS = 1000;

// ---------- Driver instances ----------
Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(PCA_ADDR);
Adafruit_INA219         ina(INA_ADDR);
ICM_20948_I2C           imu;
TinyGPSPlus             gps;
HardwareSerial          gpsSerial(2);
HardwareSerial          dfSerial(1);
DFRobot_DF1201S         df;

// ---------- Subsystem health flags (set in setup, read in loop) ----------
bool okI2C  = false;
bool okPCA  = false;
bool okINA  = false;
bool okIMU  = false;
bool okGPS  = false;
bool okDFP  = false;

unsigned long bootMs    = 0;
unsigned long lastPrint = 0;

// =====================================================================
//                              SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  bootMs = millis();

  Serial.println();
  Serial.println("============================================");
  Serial.println("  test_14_systems_check");
  Serial.println("  Post-waterproofing bringup — no motor movement");
  Serial.println("============================================");
  Serial.println();

  // ---------------- 1/6: I2C bus scan ----------------
  Serial.println("Step 1/6: I2C bus scan");
  Serial.printf("  SDA=GPIO%u  SCL=GPIO%u  freq=%lu Hz\n",
                I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_FREQ);

  bool foundPCA = false, foundINA = false, foundIMU = false, foundAllCall = false;
  int total = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      total++;
      Serial.printf("    0x%02X", addr);
      if      (addr == PCA_ADDR)    { Serial.print("  <- PCA9685");          foundPCA = true; }
      else if (addr == INA_ADDR)    { Serial.print("  <- INA219");           foundINA = true; }
      else if (addr == IMU_ADDR)    { Serial.print("  <- ICM-20948");        foundIMU = true; }
      else if (addr == PCA_ALLCALL) { Serial.print("  (PCA9685 all-call)");  foundAllCall = true; }
      Serial.println();
    }
  }
  Serial.printf("  %d device(s) found.\n", total);
  Serial.printf("    PCA9685   (0x%02X): %s\n", PCA_ADDR, foundPCA ? "FOUND" : "MISSING");
  Serial.printf("    INA219    (0x%02X): %s\n", INA_ADDR, foundINA ? "FOUND" : "MISSING");
  Serial.printf("    ICM-20948 (0x%02X): %s\n", IMU_ADDR, foundIMU ? "FOUND" : "MISSING");
  Serial.printf("    All-call  (0x%02X): %s\n", PCA_ALLCALL, foundAllCall ? "found" : "absent");

  okI2C = (foundPCA && foundINA && foundIMU);
  if (okI2C) {
    Serial.println("  PASS 1/6: all expected I2C devices present.");
  } else {
    Serial.println("  FAIL 1/6: at least one I2C device missing.");
    Serial.println("    Multimeter VCC at the missing breakout pad first —");
    Serial.println("    Wago connectors flake on small wires and an unpowered");
    Serial.println("    device still drags the bus.");
  }
  Serial.println();

  // ---------------- 2/6: PCA9685 frequency set ----------------
  Serial.println("Step 2/6: PCA9685 init (frequency set, NO output drive)");
  if (foundPCA) {
    pca.begin();
    pca.setOscillatorFrequency(27000000);   // Adafruit default; doesn't matter for ack test
    pca.setPWMFreq(50);                     // 50 Hz servo rate
    // Verify it actually accepted commands by reading MODE1 back
    Wire.beginTransmission(PCA_ADDR);
    Wire.write(0x00);                       // MODE1 register
    if (Wire.endTransmission() == 0) {
      Wire.requestFrom(PCA_ADDR, (uint8_t)1);
      if (Wire.available()) {
        uint8_t mode1 = Wire.read();
        Serial.printf("  MODE1 readback = 0x%02X\n", mode1);
        okPCA = true;
        Serial.println("  PASS 2/6: PCA9685 ACKs and accepts config.");
      } else {
        Serial.println("  FAIL 2/6: PCA9685 ACKed but did not return MODE1 byte.");
      }
    } else {
      Serial.println("  FAIL 2/6: PCA9685 did not ACK MODE1 read.");
    }
  } else {
    Serial.println("  SKIP 2/6: PCA9685 not on bus.");
  }
  Serial.println();

  // ---------------- 3/6: INA219 voltage ----------------
  Serial.println("Step 3/6: INA219 battery voltage");
  if (foundINA) {
    if (ina.begin()) {
      ina.setCalibration_32V_2A();
      // Take a few samples; first read after begin can be 0.
      float vbus = 0.0f;
      for (int i = 0; i < 5; i++) { vbus = ina.getBusVoltage_V(); delay(20); }
      Serial.printf("  Vbus = %.2f V  (expected %.1f-%.1f for 4S LiPo)\n",
                    vbus, LIPO_V_MIN, LIPO_V_MAX);
      if (vbus >= LIPO_V_MIN && vbus <= LIPO_V_MAX) {
        okINA = true;
        Serial.println("  PASS 3/6: INA219 voltage in range.");
      } else {
        Serial.println("  FAIL 3/6: voltage out of 4S range.");
        Serial.println("    Battery disconnected? VIN+/VIN- swapped? Cells damaged?");
      }
    } else {
      Serial.println("  FAIL 3/6: INA219 begin() returned false.");
    }
  } else {
    Serial.println("  SKIP 3/6: INA219 not on bus.");
  }
  Serial.println();

  // ---------------- 4/6: ICM-20948 IMU ----------------
  Serial.println("Step 4/6: ICM-20948 IMU init + accel sanity");
  if (foundIMU) {
    bool initOK = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
      imu.begin(Wire, 0);   // 0 = AD0 low → 0x68
      if (imu.status == ICM_20948_Stat_Ok) { initOK = true; break; }
      Serial.printf("  attempt %d: %s\n", attempt, imu.statusString());
      delay(300);
    }
    if (initOK) {
      // Wait briefly for first sample
      unsigned long t0 = millis();
      while (!imu.dataReady() && millis() - t0 < 500) delay(10);
      if (imu.dataReady()) {
        imu.getAGMT();
        float ax = imu.accX(), ay = imu.accY(), az = imu.accZ();
        float mag = sqrtf(ax*ax + ay*ay + az*az);
        Serial.printf("  |a| = %.0f mg  (expected %.0f-%.0f at rest)\n",
                      mag, ACCEL_MIN_MG, ACCEL_MAX_MG);
        if (mag >= ACCEL_MIN_MG && mag <= ACCEL_MAX_MG) {
          okIMU = true;
          Serial.println("  PASS 4/6: IMU returning ~1 g of accel.");
        } else {
          Serial.println("  FAIL 4/6: accel magnitude implausible. Boat moving? Sensor stuck?");
        }
      } else {
        Serial.println("  FAIL 4/6: IMU init OK but no sample ready within 500 ms.");
      }
    } else {
      Serial.println("  FAIL 4/6: IMU begin() never returned OK.");
    }
  } else {
    Serial.println("  SKIP 4/6: ICM-20948 not on bus.");
  }
  Serial.println();

  // ---------------- 5/6: GPS NMEA ----------------
  // Mirrors test_10b_bn220_gps_boat exactly: same UART2, same pins, same baud,
  // same TinyGPSPlus pump. If test_10b passed before and this fails now, the
  // delta is hardware, not software.
  Serial.println("Step 5/6: GPS NMEA parse (no fix required)");
  Serial.printf("  UART2  RX=GPIO%u  TX=GPIO%u  baud=%lu\n",
                GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  unsigned long gpsStart = millis();
  unsigned long firstByteMs = 0;
  while (millis() - gpsStart < GPS_NMEA_TIMEOUT_MS) {
    while (gpsSerial.available()) {
      int b = gpsSerial.read();
      if (firstByteMs == 0) firstByteMs = millis();
      gps.encode(b);
    }
    if (gps.passedChecksum() > 0) break;
    delay(10);
  }

  if (gps.passedChecksum() > 0) {
    okGPS = true;
    Serial.printf("  Bytes received: %lu  Sentences OK: %lu  Failed: %lu\n",
                  gps.charsProcessed(), gps.passedChecksum(), gps.failedChecksum());
    Serial.println("  PASS 5/6: GPS streaming valid NMEA. (No fix expected indoors.)");
  } else if (firstByteMs == 0) {
    Serial.println("  FAIL 5/6: no bytes from GPS at all.");
    Serial.println("    Check VCC at the BN-220 pad, GND, and TX→GPIO4 crossover.");
  } else {
    Serial.printf("  FAIL 5/6: bytes arriving (%lu) but no valid NMEA parsed.\n",
                  gps.charsProcessed());
    Serial.println("    Live loop keeps pumping — watch for RECOVERED line as you reseat wires.");
  }
  Serial.println();

  // ---------------- 6/6: DFPlayer Pro ack ----------------
  Serial.println("Step 6/6: DF1201S DFPlayer Pro UART ack");
  Serial.printf("  UART1  RX=GPIO%u  TX=GPIO%u  baud=%lu\n",
                DFP_RX_PIN, DFP_TX_PIN, DFP_BAUD);
  dfSerial.begin(DFP_BAUD, SERIAL_8N1, DFP_RX_PIN, DFP_TX_PIN);
  delay(1000);   // DF1201S needs ~1 s before begin() will succeed
  unsigned long dfpStart = millis();
  while (!df.begin(dfSerial)) {
    if (millis() - dfpStart > DFP_BEGIN_TIMEOUT_MS) {
      Serial.println("  FAIL 6/6: DF1201S did not respond to begin() within 5 s.");
      Serial.println("    Check VCC at DFPlayer pad, GND, and that ESP32-TX(26)→DFP-RX,");
      Serial.println("    ESP32-RX(25)←DFP-TX. Library must be DFRobot_DF1201S, not Mini.");
      goto dfp_done;
    }
    delay(200);
  }
  okDFP = true;
  Serial.printf("  PASS 6/6: DF1201S responded after %lu ms.\n", millis() - dfpStart);
dfp_done:
  Serial.println();

  // ---------------- Summary ----------------
  Serial.println("============================================");
  Serial.println("  SUMMARY");
  Serial.println("============================================");
  Serial.printf("  1/6 I2C scan      : %s\n", okI2C ? "PASS" : "FAIL");
  Serial.printf("  2/6 PCA9685       : %s\n", okPCA ? "PASS" : "FAIL");
  Serial.printf("  3/6 INA219 volts  : %s\n", okINA ? "PASS" : "FAIL");
  Serial.printf("  4/6 ICM-20948 IMU : %s\n", okIMU ? "PASS" : "FAIL");
  Serial.printf("  5/6 GPS NMEA      : %s\n", okGPS ? "PASS" : "FAIL");
  Serial.printf("  6/6 DFPlayer Pro  : %s\n", okDFP ? "PASS" : "FAIL");
  bool allPass = okI2C && okPCA && okINA && okIMU && okGPS && okDFP;
  Serial.println();
  if (allPass) {
    Serial.println("  ALL SYSTEMS NOMINAL — streaming 1 Hz health line.");
  } else {
    Serial.println("  ONE OR MORE FAILURES — health line still streams, fix and reset.");
  }
  Serial.println("============================================");
  Serial.println();

  lastPrint = millis();
}

// =====================================================================
//                               LOOP
// =====================================================================
void loop() {
  // Pump GPS bytes continuously so the live status line is fresh.
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  // Late GPS recovery: if the user wiggled a wire and we now have valid NMEA,
  // promote the gate from FAIL to PASS so they know their fix took.
  if (!okGPS && gps.passedChecksum() > 0) {
    okGPS = true;
    Serial.println();
    Serial.printf("  ** RECOVERED 5/6: GPS started parsing at t=%lus "
                  "(bytes=%lu, sentences=%lu). **\n",
                  (millis() - bootMs) / 1000,
                  gps.charsProcessed(), gps.passedChecksum());
    Serial.println();
  }

  if (millis() - lastPrint < PRINT_INTERVAL_MS) return;
  lastPrint = millis();

  unsigned long upS = (millis() - bootMs) / 1000;
  Serial.printf("[t=%5lus]", upS);

  // Battery
  if (okINA) {
    Serial.printf("  V=%.2fV", ina.getBusVoltage_V());
  } else {
    Serial.print("  V=---");
  }

  // IMU heading (uncalibrated, just enough to confirm it's still alive)
  if (okIMU && imu.dataReady()) {
    imu.getAGMT();
    float ax = imu.accX(), ay = imu.accY(), az = imu.accZ();
    float roll  = atan2f(ay, az) * 180.0f / PI;
    float pitch = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / PI;
    Serial.printf("  pitch=%+5.1f° roll=%+5.1f°", pitch, roll);
  } else {
    Serial.print("  IMU=---");
  }

  // GPS
  Serial.printf("  GPS=%lu/%lu",
                gps.passedChecksum(), gps.failedChecksum());
  if (gps.satellites.isValid()) {
    Serial.printf(" sats=%d", gps.satellites.value());
  }
  if (gps.location.isValid()) {
    Serial.printf(" %.5f,%.5f", gps.location.lat(), gps.location.lng());
  } else {
    Serial.print(" (no fix)");
  }

  // I2C presence retest — quick ACK-only sweep of the three expected addrs
  // so a wire that falls off mid-test shows up immediately.
  bool stillPCA = false, stillINA = false, stillIMU = false;
  Wire.beginTransmission(PCA_ADDR); stillPCA = (Wire.endTransmission() == 0);
  Wire.beginTransmission(INA_ADDR); stillINA = (Wire.endTransmission() == 0);
  Wire.beginTransmission(IMU_ADDR); stillIMU = (Wire.endTransmission() == 0);
  Serial.printf("  i2c[%c%c%c]",
                stillPCA ? 'P' : '-',
                stillINA ? 'I' : '-',
                stillIMU ? 'M' : '-');

  // DFPlayer doesn't need re-poking; if it died we'll notice on the next reset.
  Serial.printf("  DFP=%s", okDFP ? "ok" : "--");

  Serial.println();
}
