/*
 * test_23_wifi_gps_imu.ino
 *
 * Integration test: WiFi + GPS (BN-220) + IMU (ICM-20948) + complementary
 * filter heading. Extends test_21 with calibrated magnetometer and a
 * gyro/mag complementary filter that works regardless of IMU mounting
 * orientation (gravity-projected yaw rate — no hardcoded axis assumption).
 *
 * Hard-iron offsets from test_22 run 2 (2026-05-08):
 *   MAG_OFFSET_X  -38.62 µT
 *   MAG_OFFSET_Y    1.65 µT
 *   MAG_OFFSET_Z  -14.85 µT
 *
 * Complementary filter:
 *   heading = ALPHA * (heading + yawRate * dt) + (1-ALPHA) * magHeading
 *   ALPHA = 0.98 — 98% gyro (smooth), 2% mag (drift correction)
 *   yawRate = dot(gyro, accel_unit) — mounting-orientation-independent
 *
 * /telemetry adds over test_21:
 *   heading    — fused compass bearing (string, degrees)
 *   pitch      — tilt from accel (string, degrees)
 *   roll       — tilt from accel (string, degrees)
 *   accel_mag  — acceleration magnitude (int, mg). ~1000 = stationary.
 *                App uses this to suppress GPS wander when truly at rest.
 *
 * WiFi priority: home → iPhone hotspot → AP fallback "LegendCutter"
 *
 * PASS criteria:
 *   GATE 1 — WiFi connects, Serial shows IP
 *   GATE 2 — App SCAN finds boat, telemetry shows uptime incrementing
 *   GATE 3 — GPS fix acquired, position appears on map
 *   GATE 4 — Point bow at known bearing, type that number, error < 15°
 *   GATE 5 — Type 'S': heading variance < 5° over 5 s with boat held still
 *   GATE 6 — Helm screen shows heading number instead of '--'
 *
 * Hardware:
 *   ICM-20948  SDA=GPIO21  SCL=GPIO22  AD0=GND (0x68)
 *   BN-220 GPS RX=GPIO17 (white wire)  TX=GPIO4 (green wire)  9600 baud
 *   LEDs       GPIO18=nav  GPIO19=bridge  GPIO23=deck
 *
 * Libraries:
 *   "SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library"
 *   "TinyGPSPlus" by Mikal Hart
 *   "ArduinoJson" by Benoit Blanchon
 *
 * Setup:
 *   Copy secrets.h.example → secrets.h and fill in WiFi credentials.
 *   Open Serial Monitor at 115200.
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "ICM_20948.h"
#include <TinyGPSPlus.h>
#include "secrets.h"

// ── Mag calibration (test_22 run 2) ──────────────────────────────────────────
static const float MAG_OFFSET_X = -38.62f;
static const float MAG_OFFSET_Y =   1.65f;
static const float MAG_OFFSET_Z = -14.85f;

// ── Complementary filter ──────────────────────────────────────────────────────
static const float ALPHA = 0.98f;

// ── GPS config ────────────────────────────────────────────────────────────────
static const uint8_t  GPS_RX_PIN = 17;
static const uint8_t  GPS_TX_PIN = 4;
static const uint32_t GPS_BAUD   = 9600;

// ── LED pins ──────────────────────────────────────────────────────────────────
static const uint8_t PIN_NAV    = 18;
static const uint8_t PIN_BRIDGE = 19;
static const uint8_t PIN_DECK   = 23;

// ── State ─────────────────────────────────────────────────────────────────────
static ICM_20948_I2C  myICM;
static TinyGPSPlus    gps;
static HardwareSerial gpsSerial(2);
static WebServer      server(80);
static String         boatIP;

static bool   navOn = false, bridgeOn = false, deckOn = false;

// IMU live values
static float fusedHeading = 0;
static float livePitch    = 0;
static float liveRoll     = 0;
static float liveAccelMag = 0;
static unsigned long lastImuUs = 0;
static bool  headingInitialised = false;

// ── WiFi ──────────────────────────────────────────────────────────────────────
static bool tryConnect(const char* ssid, const char* pass, int timeoutSecs) {
  WiFi.begin(ssid, pass);
  Serial.printf("[WiFi] Trying %s", ssid);
  for (int i = 0; i < timeoutSecs * 2; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      boatIP = WiFi.localIP().toString();
      Serial.printf(" OK  %s\n", boatIP.c_str());
      return true;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println(" FAIL");
  WiFi.disconnect(true);
  delay(100);
  return false;
}

static void wifiSetup() {
  WiFi.mode(WIFI_STA);
  if (tryConnect(SECRET_HOME_SSID,    SECRET_HOME_PASS,    10)) return;
  if (tryConnect(SECRET_HOTSPOT_SSID, SECRET_HOTSPOT_PASS, 10)) return;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("LegendCutter", "coastguard");
  boatIP = WiFi.softAPIP().toString();
  Serial.printf("[WiFi] AP fallback  IP: %s\n", boatIP.c_str());
}

// ── IMU ───────────────────────────────────────────────────────────────────────
static void updateIMU() {
  if (!myICM.dataReady()) return;
  myICM.getAGMT();

  float ax = myICM.accX(),  ay = myICM.accY(),  az = myICM.accZ();   // mg
  float gx = myICM.gyrX(),  gy = myICM.gyrY(),  gz = myICM.gyrZ();   // deg/s
  float mx = myICM.magX(),  my = myICM.magY(),  mz = myICM.magZ();   // µT

  // Hard-iron correction
  mx -= MAG_OFFSET_X;
  my -= MAG_OFFSET_Y;
  mz -= MAG_OFFSET_Z;

  // IMU is mounted on a vertical wall with X axis pointing up (confirmed by
  // diagnostic: ax≈+984 mg constant across all yaw rotations; my/mz track
  // rotation while mx stays flat). Remap axes so standard Z-up tilt
  // compensation formulas receive the expected orientation.
  //   Remapped X = chip Y  (horizontal)
  //   Remapped Y = chip Z  (horizontal)
  //   Remapped Z = chip X  (vertical, up)
  float ar_x = ay, ar_y = az, ar_z = ax;
  float mr_x = -my, mr_y = mz, mr_z = -mx;  // chip Y=stern(negate), chip X=up(negate for tilt formula)

  // Roll / pitch from remapped accel
  liveRoll  = atan2f(ar_y, ar_z) * 180.0f / PI;
  livePitch = atan2f(-ar_x, sqrtf(ar_y*ar_y + ar_z*ar_z)) * 180.0f / PI;

  // Tilt-compensated mag heading using remapped axes
  float rR = liveRoll  * PI / 180.0f;
  float pR = livePitch * PI / 180.0f;
  float Bx = mr_x*cosf(pR) + mr_y*sinf(rR)*sinf(pR) + mr_z*cosf(rR)*sinf(pR);
  float By = mr_y*cosf(rR) - mr_z*sinf(rR);
  float magHeading = atan2f(-By, Bx) * 180.0f / PI;
  if (magHeading < 0) magHeading += 360.0f;

  // Accel magnitude (mg) — ~1000 when stationary
  liveAccelMag = sqrtf(ax*ax + ay*ay + az*az);

  // Complementary filter
  // Yaw rate: project gyro vector onto gravity direction — works for any
  // IMU mounting orientation, no hardcoded axis assumption.
  unsigned long nowUs = micros();
  float dt = (lastImuUs == 0) ? 0.0f : (nowUs - lastImuUs) * 1e-6f;
  lastImuUs = nowUs;

  if (!headingInitialised) {
    fusedHeading     = magHeading;
    headingInitialised = true;
  } else {
    float aMag   = liveAccelMag;
    float yawRate = (aMag > 100.0f)
                  ? (ax*gx + ay*gy + az*gz) / aMag   // deg/s around vertical
                  : 0.0f;                              // accel unreliable — skip
    float gyroHeading = fusedHeading + yawRate * dt;
    // Wrap to [0, 360)
    while (gyroHeading <   0.0f) gyroHeading += 360.0f;
    while (gyroHeading >= 360.0f) gyroHeading -= 360.0f;

    // Blend: 98% gyro integration, 2% mag absolute reference
    float diff = magHeading - gyroHeading;
    // Shortest-path angular difference
    if (diff >  180.0f) diff -= 360.0f;
    if (diff < -180.0f) diff += 360.0f;
    fusedHeading = gyroHeading + (1.0f - ALPHA) * diff;
    if (fusedHeading <   0.0f) fusedHeading += 360.0f;
    if (fusedHeading >= 360.0f) fusedHeading -= 360.0f;
  }
}

static void printDiag() {
  if (!myICM.dataReady()) { Serial.println("IMU not ready"); return; }
  myICM.getAGMT();
  Serial.printf("RAW  ax=%7.1f  ay=%7.1f  az=%7.1f  mg\n",
                myICM.accX(), myICM.accY(), myICM.accZ());
  Serial.printf("RAW  mx=%7.2f  my=%7.2f  mz=%7.2f  uT\n",
                myICM.magX(), myICM.magY(), myICM.magZ());
  Serial.printf("RAW  gx=%7.2f  gy=%7.2f  gz=%7.2f  deg/s\n",
                myICM.gyrX(), myICM.gyrY(), myICM.gyrZ());
}

static void runTrackingTest() {
  Serial.println("Tracking test — spin boat one full 360 deg circle in 15 s. GO.");
  float prevHeading      = fusedHeading;
  float startHeading     = fusedHeading;
  float cumulativeRotation = 0;
  unsigned long start    = millis();

  while (millis() - start < 15000) {
    updateIMU();
    server.handleClient();
    // Accumulate signed rotation step by step — distinguishes full circle
    // from 180-out-and-back (which nets to ~0 cumulative).
    float delta = fusedHeading - prevHeading;
    if (delta >  180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;
    cumulativeRotation += delta;
    prevHeading = fusedHeading;
    delay(10);
  }

  float endHeading  = fusedHeading;
  float returnError = fabsf(endHeading - startHeading);
  if (returnError > 180.0f) returnError = 360.0f - returnError;
  float absCumulative = fabsf(cumulativeRotation);

  Serial.printf("Start: %.1f deg  End: %.1f deg  Cumulative: %.1f deg  Return error: %.1f deg\n",
                startHeading, endHeading, cumulativeRotation, returnError);
  if (absCumulative >= 300.0f && returnError <= 20.0f)
    Serial.println("[TRACKING] PASS — full rotation confirmed, closed within 20 deg.");
  else if (absCumulative < 300.0f)
    Serial.printf("[TRACKING] FAIL — only %.1f deg of cumulative rotation (need 300+). Spin further.\n",
                  absCumulative);
  else
    Serial.println("[TRACKING] FAIL — full rotation detected but did not return within 20 deg of start.");
}

static void runGate4(int reference) {
  float err = fusedHeading - (float)reference;
  if (err >  180.0f) err -= 360.0f;
  if (err < -180.0f) err += 360.0f;
  float absErr = fabsf(err);
  Serial.printf("IMU heading: %.1f°  Reference: %d°  Error: %.1f°  ",
                fusedHeading, reference, absErr);
  if (absErr <= 15.0f)
    Serial.println("[GATE 4] PASS");
  else
    Serial.println("[GATE 4] FAIL — error > 15deg. Check mag offsets or redo test_22.");
}

static void runGate5() {
  Serial.println("Stability test — hold boat still for 5 seconds...");
  float minH = fusedHeading, maxH = fusedHeading;
  unsigned long start = millis();
  while (millis() - start < 5000) {
    updateIMU();
    server.handleClient();
    if (fusedHeading < minH) minH = fusedHeading;
    if (fusedHeading > maxH) maxH = fusedHeading;
    delay(10);
  }
  float range = maxH - minH;
  // Unwrap: if readings cross 0/360 boundary the naive range will be huge
  if (range > 180.0f) range = 360.0f - range;
  Serial.printf("Min: %.1f°  Max: %.1f°  Range: %.1f°  ", minH, maxH, range);
  if (range <= 5.0f)
    Serial.println("[GATE 5] PASS");
  else
    Serial.println("[GATE 5] FAIL — drift > 5deg. Boat may have moved or ALPHA needs tuning.");
}

// ── HTTP ──────────────────────────────────────────────────────────────────────
static void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void handleOptions() { addCORS(); server.send(204); }

static void handleStatus() {
  addCORS();
  String body = "{\"ok\":true,\"v\":\"test_23\",\"ip\":\"" + boatIP + "\"}";
  server.send(200, "application/json", body);
}

static void handleTelemetry() {
  addCORS();
  StaticJsonDocument<512> doc;
  doc["v"]         = "test_23";
  doc["uptime"]    = millis() / 1000;
  doc["heap"]      = ESP.getFreeHeap();
  doc["nav_on"]    = navOn;
  doc["bridge_on"] = bridgeOn;
  doc["deck_on"]   = deckOn;

  // IMU
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", fusedHeading); doc["heading"]   = buf;
  snprintf(buf, sizeof(buf), "%.1f", livePitch);    doc["pitch"]     = buf;
  snprintf(buf, sizeof(buf), "%.1f", liveRoll);     doc["roll"]      = buf;
  doc["accel_mag"] = (int)liveAccelMag;

  // GPS
  bool hasFix = gps.location.isValid() && gps.location.age() < 5000;
  doc["gps_fix"] = hasFix;
  doc["sats"]    = (int)gps.satellites.value();
  if (hasFix) {
    snprintf(buf, sizeof(buf), "%.6f", gps.location.lat()); doc["lat"] = buf;
    snprintf(buf, sizeof(buf), "%.6f", gps.location.lng()); doc["lon"] = buf;
    snprintf(buf, sizeof(buf), "%.1f", gps.speed.knots());  doc["speed_kts"] = buf;
    snprintf(buf, sizeof(buf), "%.1f", gps.course.deg());   doc["course"]    = buf;
  }

  char out[512];
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleLed() {
  addCORS();
  if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
  if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

  StaticJsonDocument<128> req;
  if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

  const char* light = req["light"] | "";
  bool        on    = req["state"]  | false;
  uint8_t     pin   = 0;
  bool*       state = nullptr;

  if      (strcmp(light, "nav")    == 0) { pin = PIN_NAV;    state = &navOn;    }
  else if (strcmp(light, "bridge") == 0) { pin = PIN_BRIDGE; state = &bridgeOn; }
  else if (strcmp(light, "deck")   == 0) { pin = PIN_DECK;   state = &deckOn;   }
  else { server.send(400, "text/plain", "Unknown light"); return; }

  *state = on;
  digitalWrite(pin, on ? HIGH : LOW);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("test_23_wifi_gps_imu");

  pinMode(PIN_NAV,    OUTPUT); digitalWrite(PIN_NAV,    LOW);
  pinMode(PIN_BRIDGE, OUTPUT); digitalWrite(PIN_BRIDGE, LOW);
  pinMode(PIN_DECK,   OUTPUT); digitalWrite(PIN_DECK,   LOW);

  Wire.begin(21, 22);
  Wire.setClock(400000);

  bool imuOK = false;
  for (int i = 1; i <= 3; i++) {
    myICM.begin(Wire, 0);
    if (myICM.status == ICM_20948_Stat_Ok) { imuOK = true; break; }
    delay(500);
  }
  if (!imuOK) {
    Serial.println("[FAIL] ICM-20948 init failed. Check SDA=21 SCL=22 AD0=GND.");
    while (true) delay(1000);
  }
  Serial.println("[IMU] ICM-20948 ready.");

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.printf("[GPS] UART2 RX=GPIO%d TX=GPIO%d @ %d baud\n",
                GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

  wifiSetup();
  Serial.println("[GATE 1] PASS candidate — see IP above");

  server.on("/status",    HTTP_GET,     handleStatus);
  server.on("/status",    HTTP_OPTIONS, handleOptions);
  server.on("/telemetry", HTTP_GET,     handleTelemetry);
  server.on("/telemetry", HTTP_OPTIONS, handleOptions);
  server.on("/led",       HTTP_POST,    handleLed);
  server.on("/led",       HTTP_OPTIONS, handleOptions);
  server.begin();
  Serial.printf("[HTTP] port 80  /status  /telemetry  /led  (IP: %s)\n", boatIP.c_str());
  Serial.println("Ready.");
  Serial.println("  GATE 4: point bow at a known bearing, type that number + enter.");
  Serial.println("  GATE 5: type S + enter with boat held still.");
  Serial.println("  DIAG:   type D to print raw accel/mag/gyro.");
  Serial.println("  TRACK:  type T then spin boat one full circle (15 s).");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
static char   serialBuf[8];
static uint8_t serialLen = 0;

void loop() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());
  updateIMU();
  server.handleClient();

  // Buffer Serial input until newline, then parse command or bearing number
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLen > 0) {
        serialBuf[serialLen] = '\0';
        if (serialBuf[0] == 'S' || serialBuf[0] == 's') {
          runGate5();
        } else if (serialBuf[0] == 'D' || serialBuf[0] == 'd') {
          printDiag();
        } else if (serialBuf[0] == 'T' || serialBuf[0] == 't') {
          runTrackingTest();
        } else {
          int ref = atoi(serialBuf);
          if (ref >= 0 && ref <= 360) {
            runGate4(ref);
          } else {
            Serial.println("Enter a compass bearing (0-360) or S for stability test.");
          }
        }
        serialLen = 0;
      }
    } else if (serialLen < sizeof(serialBuf) - 1) {
      serialBuf[serialLen++] = c;
    }
  }
}
