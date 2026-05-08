/*
 * test_22_mag_calibration.ino
 *
 * Magnetometer hard-iron calibration for the ICM-20948.
 *
 * On boot: 5-second countdown, then a 30-second silent spin window.
 * Spin the boat through a full 360° in the horizontal plane, 2-3 times.
 * When the window closes, Serial prints the results and STOPS — no live stream.
 * Send 'H' in Serial Monitor any time after calibration to print a single
 * heading snapshot.
 *
 * PASS criteria:
 *   GATE 1 — Serial shows WiFi connected + IP
 *   GATE 2 — 30-second spin window completes without error
 *   GATE 3 — Results show rangeX and rangeY both > 20 µT
 *   GATE 4 — Send 'H', heading matches known compass bearing ±15°
 *   GATE 5 — Send 'H' twice with boat held still, readings differ < 5°
 *
 * Wiring:
 *   ICM-20948 SDA → GPIO 21
 *   ICM-20948 SCL → GPIO 22
 *   ICM-20948 AD0 → GND (address 0x68)
 *
 * Libraries:
 *   "SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library"
 *   "ArduinoJson" by Benoit Blanchon
 *
 * Setup:
 *   Copy secrets.h.example → secrets.h, fill in WiFi credentials.
 *   Open Serial Monitor at 115200 baud.
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "ICM_20948.h"
#include "secrets.h"

static const unsigned long CAL_SPIN_MS = 30000;

static ICM_20948_I2C myICM;
static String        boatIP;
static WebServer     server(80);

static float offsetX = 0, offsetY = 0, offsetZ = 0;
static bool  calDone = false;

static float liveHeading  = 0;
static float livePitch    = 0;
static float liveRoll     = 0;
static float liveAccelMag = 0;

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
static void readAGMT(float &ax, float &ay, float &az,
                     float &mx, float &my, float &mz) {
  myICM.getAGMT();
  ax = myICM.accX();
  ay = myICM.accY();
  az = myICM.accZ();
  mx = myICM.magX();
  my = myICM.magY();
  mz = myICM.magZ();
}

static void computeAttitude(float ax, float ay, float az,
                             float mx, float my, float mz,
                             float &heading, float &pitch, float &roll) {
  roll  = atan2f(ay, az) * 180.0f / PI;
  pitch = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / PI;
  float rR = roll  * PI / 180.0f;
  float pR = pitch * PI / 180.0f;
  float Bx = mx * cosf(pR) + my * sinf(rR)*sinf(pR) + mz * cosf(rR)*sinf(pR);
  float By = my * cosf(rR) - mz * sinf(rR);
  heading  = atan2f(-By, Bx) * 180.0f / PI;
  if (heading < 0) heading += 360.0f;
}

static void printHeadingSnapshot() {
  if (!myICM.dataReady()) { Serial.println("IMU not ready"); return; }
  float ax, ay, az, mx, my, mz;
  readAGMT(ax, ay, az, mx, my, mz);
  computeAttitude(ax, ay, az, mx - offsetX, my - offsetY, mz - offsetZ,
                  liveHeading, livePitch, liveRoll);
  liveAccelMag = sqrtf(ax*ax + ay*ay + az*az);
  Serial.printf("Heading: %.1f°   Pitch: %+.1f°   Roll: %+.1f°   |a|=%.0f mg\n",
                liveHeading, livePitch, liveRoll, liveAccelMag);
}

// ── Calibration ───────────────────────────────────────────────────────────────
static void runCalibration() {
  Serial.println();
  Serial.println("Magnetometer calibration — spin the boat through a full");
  Serial.println("360 deg horizontal circle, 2-3 times over 30 seconds.");
  Serial.println("Tilt bow up ~30 deg and spin again for best accuracy.");
  Serial.println("Keep away from speakers, motors, and steel.");
  Serial.println();

  for (int s = 5; s >= 1; s--) {
    Serial.printf("Starting in %d...\n", s);
    delay(1000);
  }
  Serial.println("SPIN NOW");

  float minX =  1e9, minY =  1e9, minZ =  1e9;
  float maxX = -1e9, maxY = -1e9, maxZ = -1e9;
  int   sampleCnt = 0;
  unsigned long spinStart = millis();

  while (millis() - spinStart < CAL_SPIN_MS) {
    if (myICM.dataReady()) {
      float ax, ay, az, mx, my, mz;
      readAGMT(ax, ay, az, mx, my, mz);
      if (mx < minX) minX = mx;  if (mx > maxX) maxX = mx;
      if (my < minY) minY = my;  if (my > maxY) maxY = my;
      if (mz < minZ) minZ = mz;  if (mz > maxZ) maxZ = mz;
      sampleCnt++;
    }
    server.handleClient();
    delay(10);
  }

  offsetX = (minX + maxX) / 2.0f;
  offsetY = (minY + maxY) / 2.0f;
  offsetZ = (minZ + maxZ) / 2.0f;
  calDone = true;

  float rangeX = maxX - minX;
  float rangeY = maxY - minY;
  float rangeZ = maxZ - minZ;

  Serial.println();
  Serial.println("========================================");
  Serial.println("CALIBRATION DONE");
  Serial.printf("Samples: %d\n", sampleCnt);
  Serial.println();
  Serial.printf("Axis  min      max      range    offset\n");
  Serial.printf("X   %7.2f  %7.2f  %6.2f  %7.2f\n", minX, maxX, rangeX, offsetX);
  Serial.printf("Y   %7.2f  %7.2f  %6.2f  %7.2f\n", minY, maxY, rangeY, offsetY);
  Serial.printf("Z   %7.2f  %7.2f  %6.2f  %7.2f\n", minZ, maxZ, rangeZ, offsetZ);
  Serial.println();

  if (rangeX < 20.0f || rangeY < 20.0f) {
    Serial.println("[WARN] rangeX or rangeY < 20 uT — spin was incomplete or");
    Serial.println("       magnetic interference present. Reflash and redo.");
  } else {
    Serial.println("[GATE 3] PASS — spin range looks good.");
  }

  Serial.println();
  Serial.println("Copy into test_23:");
  Serial.printf("#define MAG_OFFSET_X  %.2ff\n", offsetX);
  Serial.printf("#define MAG_OFFSET_Y  %.2ff\n", offsetY);
  Serial.printf("#define MAG_OFFSET_Z  %.2ff\n", offsetZ);
  Serial.println("========================================");
  Serial.println("Send 'H' for a heading snapshot. [GATE 4/5]");
}

// ── HTTP ──────────────────────────────────────────────────────────────────────
static void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void handleStatus() {
  addCORS();
  String body = "{\"ok\":true,\"v\":\"test_22\",\"ip\":\"" + boatIP
              + "\",\"cal_done\":" + (calDone ? "true" : "false") + "}";
  server.send(200, "application/json", body);
}

static void handleTelemetry() {
  addCORS();
  StaticJsonDocument<256> doc;
  doc["v"]        = "test_22";
  doc["uptime"]   = millis() / 1000;
  doc["heap"]     = ESP.getFreeHeap();
  doc["cal_done"] = calDone;
  if (calDone) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", liveHeading);  doc["heading"]   = buf;
    snprintf(buf, sizeof(buf), "%.1f", livePitch);    doc["pitch"]     = buf;
    snprintf(buf, sizeof(buf), "%.1f", liveRoll);     doc["roll"]      = buf;
    doc["accel_mag"] = (int)liveAccelMag;
  }
  char out[256];
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleOptions() { addCORS(); server.send(204); }

// ── Setup / Loop ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("test_22_mag_calibration");

  Wire.begin(21, 22);
  Wire.setClock(400000);

  bool initOK = false;
  for (int i = 1; i <= 3; i++) {
    myICM.begin(Wire, 0);
    if (myICM.status == ICM_20948_Stat_Ok) { initOK = true; break; }
    delay(500);
  }
  if (!initOK) {
    Serial.println("[FAIL] ICM-20948 init failed. Check SDA=21 SCL=22 AD0=GND.");
    while (true) delay(1000);
  }

  wifiSetup();
  Serial.println("[GATE 1] PASS");

  server.on("/status",    HTTP_GET,     handleStatus);
  server.on("/status",    HTTP_OPTIONS, handleOptions);
  server.on("/telemetry", HTTP_GET,     handleTelemetry);
  server.on("/telemetry", HTTP_OPTIONS, handleOptions);
  server.begin();

  runCalibration();
}

void loop() {
  server.handleClient();

  // Keep live values fresh for HTTP /telemetry (silent — no Serial output)
  if (calDone && myICM.dataReady()) {
    float ax, ay, az, mx, my, mz;
    readAGMT(ax, ay, az, mx, my, mz);
    computeAttitude(ax, ay, az, mx - offsetX, my - offsetY, mz - offsetZ,
                    liveHeading, livePitch, liveRoll);
    liveAccelMag = sqrtf(ax*ax + ay*ay + az*az);
  }

  // 'H' in Serial Monitor → single heading snapshot
  if (Serial.available() && Serial.read() == 'H') {
    printHeadingSnapshot();
  }
}
