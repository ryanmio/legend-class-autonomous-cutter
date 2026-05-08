/*
 * test_22_mag_calibration.ino
 *
 * Two-phase sketch:
 *
 *  PHASE 1 — CALIBRATION (runs once on boot)
 *    5-second warning, then a 30-second spin window.
 *    Spin the boat slowly through 360° in the horizontal plane, 2-3 times.
 *    Optionally tilt bow up ~30° and spin again. Tilt stern up and spin again.
 *    At the end, Serial prints three #define lines — copy them into test_23.
 *
 *  PHASE 2 — LIVE HEADING (runs after calibration)
 *    Tilt-compensated heading using the offsets just collected.
 *    Also streams accel_mag (useful later for ZUPT / motion detection).
 *    GET /telemetry returns heading, pitch, roll, accel_mag over HTTP.
 *
 * PASS criteria:
 *   GATE 1 — Serial shows "Connected" + IP
 *   GATE 2 — Serial shows raw mag values changing smoothly as you rotate
 *   GATE 3 — 30-second spin window completes; Serial prints #define offsets
 *   GATE 4 — Post-calibration heading tracks your known compass bearing ±15°
 *   GATE 5 — Heading stays stable (< 5° drift) when boat held still for 10 s
 *
 * Wiring:
 *   ICM-20948 SDA → GPIO 21
 *   ICM-20948 SCL → GPIO 22
 *   ICM-20948 AD0 → GND (address 0x68)
 *
 * Libraries:
 *   "SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library" (Library Manager)
 *   "ArduinoJson" by Benoit Blanchon
 *
 * Setup:
 *   Copy secrets.h.example → secrets.h and fill in WiFi credentials.
 *   Open Serial Monitor at 115200.
 *   Flash and watch. Hands free during the spin window.
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "ICM_20948.h"
#include "secrets.h"

// ── Config ────────────────────────────────────────────────────────────────────
static const unsigned long CAL_WARN_MS = 5000;   // countdown before spin starts
static const unsigned long CAL_SPIN_MS = 30000;  // spin window duration
static const unsigned long PRINT_MS    = 2000;   // live heading print rate (0.5 Hz)

// ── State ─────────────────────────────────────────────────────────────────────
static ICM_20948_I2C myICM;
static String        boatIP;
static WebServer     server(80);

// Calibration results (populated at end of phase 1)
static float offsetX = 0, offsetY = 0, offsetZ = 0;
static bool  calDone = false;

// Live heading (updated every loop)
static float liveHeading  = 0;
static float livePitch    = 0;
static float liveRoll     = 0;
static float liveAccelMag = 0;

// ── WiFi ──────────────────────────────────────────────────────────────────────
static bool tryConnect(const char* ssid, const char* pass, int timeoutSecs) {
  WiFi.begin(ssid, pass);
  Serial.printf("[WiFi] Trying %-28s", ssid);
  for (int i = 0; i < timeoutSecs * 2; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      boatIP = WiFi.localIP().toString();
      Serial.printf("  OK  %s\n", boatIP.c_str());
      return true;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("  FAIL");
  WiFi.disconnect(true);
  delay(100);
  return false;
}

static void wifiSetup() {
  WiFi.mode(WIFI_STA);
  if (tryConnect(SECRET_HOME_SSID,    SECRET_HOME_PASS,    10)) return;
  if (tryConnect(SECRET_HOTSPOT_SSID, SECRET_HOTSPOT_PASS, 10)) return;
  Serial.println("[WiFi] No known network — starting AP fallback");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("LegendCutter", "coastguard");
  boatIP = WiFi.softAPIP().toString();
  Serial.printf("[WiFi] AP  SSID: LegendCutter  IP: %s\n", boatIP.c_str());
}

// ── IMU helpers ───────────────────────────────────────────────────────────────
static void readAGMT(float &ax, float &ay, float &az,
                     float &mx, float &my, float &mz) {
  myICM.getAGMT();
  ax = myICM.accX();  // mg
  ay = myICM.accY();
  az = myICM.accZ();
  mx = myICM.magX();  // µT
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
  float Bx = mx * cosf(pR)
           + my * sinf(rR) * sinf(pR)
           + mz * cosf(rR) * sinf(pR);
  float By = my * cosf(rR) - mz * sinf(rR);
  heading = atan2f(-By, Bx) * 180.0f / PI;
  if (heading < 0) heading += 360.0f;
}

// ── Phase 1: calibration ──────────────────────────────────────────────────────
static void runCalibration() {
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════╗");
  Serial.println("║         MAGNETOMETER CALIBRATION             ║");
  Serial.println("╠══════════════════════════════════════════════╣");
  Serial.println("║  In 5 seconds the 30-second spin window      ║");
  Serial.println("║  will begin. Do this:                        ║");
  Serial.println("║                                              ║");
  Serial.println("║  1. Slowly rotate the boat through a full   ║");
  Serial.println("║     360° in the horizontal plane. Do it      ║");
  Serial.println("║     2-3 times over 30 seconds.               ║");
  Serial.println("║                                              ║");
  Serial.println("║  2. Optional (better accuracy): tilt the     ║");
  Serial.println("║     bow up ~30° and do a slow spin, then     ║");
  Serial.println("║     tilt the stern up and spin again.        ║");
  Serial.println("║                                              ║");
  Serial.println("║  Keep away from steel, speakers, the motor.  ║");
  Serial.println("║  Hands free — watch Serial, not the boat.    ║");
  Serial.println("╚══════════════════════════════════════════════╝");
  Serial.println();

  for (int s = 5; s >= 1; s--) {
    Serial.printf("  Starting in %d...\n", s);
    delay(1000);
  }

  Serial.println();
  Serial.println(">>> SPIN NOW <<<");
  Serial.println();

  float minX =  1e9, minY =  1e9, minZ =  1e9;
  float maxX = -1e9, maxY = -1e9, maxZ = -1e9;

  unsigned long spinStart  = millis();
  unsigned long lastPrint  = 0;
  int           sampleCnt  = 0;
  int           lastSecPrinted = -1;

  while (millis() - spinStart < CAL_SPIN_MS) {
    if (!myICM.dataReady()) { delay(5); continue; }

    float ax, ay, az, mx, my, mz;
    readAGMT(ax, ay, az, mx, my, mz);

    if (mx < minX) minX = mx;  if (mx > maxX) maxX = mx;
    if (my < minY) minY = my;  if (my > maxY) maxY = my;
    if (mz < minZ) minZ = mz;  if (mz > maxZ) maxZ = mz;
    sampleCnt++;

    // One line per second: time remaining + growing range (plateaus when spin is complete)
    int secRemaining = (int)((CAL_SPIN_MS - (millis() - spinStart)) / 1000);
    if (secRemaining != lastSecPrinted) {
      lastSecPrinted = secRemaining;
      Serial.printf("  [%2ds]  rangeX=%5.1f  rangeY=%5.1f  rangeZ=%5.1f\n",
                    secRemaining, maxX - minX, maxY - minY, maxZ - minZ);
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
  Serial.println("  CALIBRATION COMPLETE");
  Serial.printf("  Samples collected: %d\n", sampleCnt);
  Serial.println();
  Serial.printf("  Axis   min      max      range    offset\n");
  Serial.printf("  X   %7.2f  %7.2f  %7.2f  %7.2f\n", minX, maxX, rangeX, offsetX);
  Serial.printf("  Y   %7.2f  %7.2f  %7.2f  %7.2f\n", minY, maxY, rangeY, offsetY);
  Serial.printf("  Z   %7.2f  %7.2f  %7.2f  %7.2f\n", minZ, maxZ, rangeZ, offsetZ);
  Serial.println();

  // Quality check: a healthy spin should produce at least 20 µT range on X and Y
  if (rangeX < 20.0f || rangeY < 20.0f) {
    Serial.println("  [WARN] Range on X or Y axis is small — did the boat complete");
    Serial.println("         a full 360° spin? Consider re-flashing and trying again.");
    Serial.println("         Also check for nearby magnetic interference.");
  } else {
    Serial.println("  [GATE 3] Spin range looks good.");
  }

  Serial.println();
  Serial.println("  Copy these three lines into test_23 (or config.h):");
  Serial.println("  ─────────────────────────────────────────────────");
  Serial.printf("  #define MAG_OFFSET_X  %.2ff\n", offsetX);
  Serial.printf("  #define MAG_OFFSET_Y  %.2ff\n", offsetY);
  Serial.printf("  #define MAG_OFFSET_Z  %.2ff\n", offsetZ);
  Serial.println("  ─────────────────────────────────────────────────");
  Serial.println();
  Serial.println("  Switching to LIVE HEADING mode...");
  Serial.println("========================================");
  Serial.println();
}

// ── HTTP handlers ─────────────────────────────────────────────────────────────
static void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void handleStatus() {
  addCORS();
  String body = "{\"ok\":true,\"v\":\"test_22\",\"ip\":\"" + boatIP + "\",\"cal_done\":"
              + (calDone ? "true" : "false") + "}";
  server.send(200, "application/json", body);
}

static void handleTelemetry() {
  addCORS();
  StaticJsonDocument<256> doc;
  doc["v"]         = "test_22";
  doc["uptime"]    = millis() / 1000;
  doc["heap"]      = ESP.getFreeHeap();
  doc["cal_done"]  = calDone;

  if (calDone) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", liveHeading);
    doc["heading"] = buf;
    snprintf(buf, sizeof(buf), "%.1f", livePitch);
    doc["pitch"] = buf;
    snprintf(buf, sizeof(buf), "%.1f", liveRoll);
    doc["roll"] = buf;
    // accel_mag in mg — ~1000 = stationary, deviates when moving
    doc["accel_mag"] = (int)liveAccelMag;
  }

  char out[256];
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleOptions() {
  addCORS();
  server.send(204);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_22_mag_calibration");
  Serial.println("========================================");

  Wire.begin(21, 22);
  Wire.setClock(400000);

  Serial.println("[IMU] Initializing ICM-20948...");
  bool initOK = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    myICM.begin(Wire, 0);
    if (myICM.status == ICM_20948_Stat_Ok) { initOK = true; break; }
    Serial.printf("  attempt %d: %s\n", attempt, myICM.statusString());
    delay(500);
  }
  if (!initOK) {
    Serial.println("[FAIL] ICM-20948 init failed. Check SDA=21, SCL=22, AD0=GND.");
    while (true) delay(1000);
  }
  Serial.println("[IMU] ICM-20948 ready.");

  wifiSetup();
  Serial.println("[GATE 1] PASS candidate — see IP above");

  server.on("/status",    HTTP_GET,     handleStatus);
  server.on("/status",    HTTP_OPTIONS, handleOptions);
  server.on("/telemetry", HTTP_GET,     handleTelemetry);
  server.on("/telemetry", HTTP_OPTIONS, handleOptions);
  server.begin();
  Serial.printf("[HTTP] port 80  /status  /telemetry  (IP: %s)\n", boatIP.c_str());

  // Small delay so Serial output settles before calibration banner
  delay(500);

  runCalibration();
}

// ── Loop: live heading ────────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  if (!myICM.dataReady()) return;

  float ax, ay, az, mx, my, mz;
  readAGMT(ax, ay, az, mx, my, mz);

  // Apply hard-iron offsets
  float mxc = mx - offsetX;
  float myc = my - offsetY;
  float mzc = mz - offsetZ;

  computeAttitude(ax, ay, az, mxc, myc, mzc,
                  liveHeading, livePitch, liveRoll);

  liveAccelMag = sqrtf(ax*ax + ay*ay + az*az);

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= PRINT_MS) {
    lastPrint = millis();
    Serial.printf("Heading: %6.1f°   Pitch: %+6.1f°   Roll: %+6.1f°   |a|=%4.0f mg\n",
                  liveHeading, livePitch, liveRoll, liveAccelMag);
  }
}
