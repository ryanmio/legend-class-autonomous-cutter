/*
 * test_30_mag_calibration.ino
 *
 * App-triggered, NVS-persisted mag calibration for the ICM-20948.
 * Proves the plumbing that test_29 will inherit:
 *   - Plateau-detect cal procedure (ported from test_22) runs onboard
 *     when triggered via HTTP, not at boot.
 *   - Resulting offsets persist in ESP32 NVS (Preferences) namespace
 *     `imu_cal` and survive power cycles.
 *   - Live heading uses NVS-loaded offsets with the test_29 axis-remap
 *     and tilt-compensated convention so values port directly.
 *   - Telemetry exposes cal_state, progress, offsets, magnitude.
 *
 * Bench-only sketch — no motors, GPS, audio, bilge. Operator runs cal
 * by POSTing /calibrate_mag/start, spinning the board through 360°,
 * watching state via /telemetry, then power-cycles to confirm
 * persistence and re-checks heading.
 *
 * Pass gates — see NOTES.md.
 *
 * Wiring:
 *   ICM-20948 SDA → GPIO 21
 *   ICM-20948 SCL → GPIO 22
 *   ICM-20948 AD0 → GND (address 0x68)
 *
 * Setup:
 *   Copy secrets.h.example → secrets.h, fill in WiFi credentials.
 *   Open Serial Monitor at 115200 baud. App optional — curl works.
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_system.h>
#include "ICM_20948.h"
#include "secrets.h"

// ── Cal tuning (matches test_22) ──────────────────────────────────────────
static const unsigned long CAL_MIN_MS      = 10000;  // must spin at least this long
static const unsigned long CAL_TIMEOUT_MS  = 60000;  // app-driven cal cap
static const float         CAL_MIN_RANGE   = 20.0f;  // µT — minimum credible per-axis range
static const float         CAL_PLATEAU_UT  = 1.0f;   // µT growth in 5 s = still spinning
static const int           CAL_HIST        = 5;      // ring-buffer slots, 1 per sec

// ── State machine ─────────────────────────────────────────────────────────
enum CalState { CAL_IDLE, CAL_COLLECTING, CAL_DONE, CAL_FAILED };
static CalState   calState        = CAL_IDLE;
static const char* calStateName(CalState s) {
    return s == CAL_IDLE       ? "idle"
         : s == CAL_COLLECTING ? "collecting"
         : s == CAL_DONE       ? "done"
                               : "failed";
}

// Cal-in-progress scratch
static float minX, minY, minZ, maxX, maxY, maxZ;
static int   sampleCnt = 0;
static unsigned long calStartMs = 0;
static float histX[CAL_HIST] = {}, histY[CAL_HIST] = {};
static int   histIdx = 0;
static unsigned long lastHistMs = 0;
static const char* failReason = "";

// Persistent cal (loaded from NVS on boot, rewritten on cal success)
static float    magOffX = 0, magOffY = 0, magOffZ = 0;
static float    magBaselineUT = 0;
static uint32_t magCalTs = 0;     // millis()/1000 at cal time; cosmetic

// Live readings (updated every loop)
static float liveMagX = 0, liveMagY = 0, liveMagZ = 0;
static float liveAccX = 0, liveAccY = 0, liveAccZ = 0;
static float liveHeading = 0;
static float liveMagUT = 0;

// Hardware-random session id so the app can detect mid-flight reboots
static uint32_t sessionId = 0;

static ICM_20948_I2C myICM;
static String        boatIP;
static WebServer     server(80);
static Preferences   prefs;

// ── WiFi ──────────────────────────────────────────────────────────────────
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
    Serial.println("[WiFi] no network — endpoints unreachable, cal can still be triggered via Serial if needed.");
}

// ── NVS ───────────────────────────────────────────────────────────────────
static void loadCalFromNVS() {
    prefs.begin("imu_cal", true);   // readonly
    magOffX       = prefs.getFloat("off_x", 0.0f);
    magOffY       = prefs.getFloat("off_y", 0.0f);
    magOffZ       = prefs.getFloat("off_z", 0.0f);
    magBaselineUT = prefs.getFloat("base_uT", 0.0f);
    magCalTs      = prefs.getUInt("cal_ts", 0);
    prefs.end();
    Serial.printf("[NVS] loaded cal: off=(%.2f,%.2f,%.2f) base=%.2f uT cal_ts=%u\n",
                  magOffX, magOffY, magOffZ, magBaselineUT, magCalTs);
}

static void saveCalToNVS() {
    prefs.begin("imu_cal", false);
    prefs.putFloat("off_x", magOffX);
    prefs.putFloat("off_y", magOffY);
    prefs.putFloat("off_z", magOffZ);
    prefs.putFloat("base_uT", magBaselineUT);
    prefs.putUInt ("cal_ts", magCalTs);
    prefs.end();
    Serial.printf("[NVS] saved cal: off=(%.2f,%.2f,%.2f) base=%.2f uT\n",
                  magOffX, magOffY, magOffZ, magBaselineUT);
}

static bool magCalibrated() {
    return magBaselineUT > 0.0f
        && (magOffX != 0.0f || magOffY != 0.0f || magOffZ != 0.0f);
}

// ── IMU read + heading (matches test_29 convention) ──────────────────────
static bool readIMU() {
    if (!myICM.dataReady()) return false;
    myICM.getAGMT();
    liveAccX = myICM.accX(); liveAccY = myICM.accY(); liveAccZ = myICM.accZ();
    liveMagX = myICM.magX(); liveMagY = myICM.magY(); liveMagZ = myICM.magZ();
    return true;
}

// Apply NVS offsets, then test_29 axis remap (chip X=up, Y=port, Z=stern),
// then tilt-compensated atan2. Mirror test_29 exactly so cal values port.
static void recomputeHeading() {
    float mx = liveMagX - magOffX;
    float my = liveMagY - magOffY;
    float mz = liveMagZ - magOffZ;

    float ar_x = liveAccY,  ar_y = liveAccZ,  ar_z = liveAccX;
    float mr_x = -mz,       mr_y = -my,       mr_z = -mx;
    float roll  = atan2f(ar_y, ar_z);
    float pitch = atan2f(-ar_x, sqrtf(ar_y*ar_y + ar_z*ar_z));
    float Bx = mr_x*cosf(pitch) + mr_y*sinf(roll)*sinf(pitch) + mr_z*cosf(roll)*sinf(pitch);
    float By = mr_y*cosf(roll)  - mr_z*sinf(roll);
    float h  = atan2f(-By, Bx) * 180.0f / PI;
    if (h < 0) h += 360.0f;
    liveHeading = h;

    liveMagUT = sqrtf(mx*mx + my*my + mz*mz);
}

// ── Cal procedure ─────────────────────────────────────────────────────────
static void calBegin() {
    minX =  1e9f; minY =  1e9f; minZ =  1e9f;
    maxX = -1e9f; maxY = -1e9f; maxZ = -1e9f;
    sampleCnt = 0;
    histIdx = 0;
    lastHistMs = 0;
    for (int i = 0; i < CAL_HIST; i++) { histX[i] = 0; histY[i] = 0; }
    calStartMs = millis();
    failReason = "";
    calState = CAL_COLLECTING;
    Serial.println("[CAL] start — spin boat through 360°");
}

static void calAbort(const char* why) {
    failReason = why;
    calState = CAL_FAILED;
    Serial.printf("[CAL] aborted: %s\n", why);
}

static void calSuccess() {
    magOffX = (minX + maxX) / 2.0f;
    magOffY = (minY + maxY) / 2.0f;
    magOffZ = (minZ + maxZ) / 2.0f;
    // Baseline magnitude post-offset, captured at the final boat orientation.
    float mx = liveMagX - magOffX;
    float my = liveMagY - magOffY;
    float mz = liveMagZ - magOffZ;
    magBaselineUT = sqrtf(mx*mx + my*my + mz*mz);
    magCalTs = millis() / 1000;
    saveCalToNVS();
    calState = CAL_DONE;
    Serial.printf("[CAL] done — samples=%d off=(%.2f,%.2f,%.2f) base=%.2f uT\n",
                  sampleCnt, magOffX, magOffY, magOffZ, magBaselineUT);
}

static void calTick() {
    if (calState != CAL_COLLECTING) return;
    if (!readIMU()) return;

    if (liveMagX < minX) minX = liveMagX;  if (liveMagX > maxX) maxX = liveMagX;
    if (liveMagY < minY) minY = liveMagY;  if (liveMagY > maxY) maxY = liveMagY;
    if (liveMagZ < minZ) minZ = liveMagZ;  if (liveMagZ > maxZ) maxZ = liveMagZ;
    sampleCnt++;

    unsigned long elapsed = millis() - calStartMs;

    // Snapshot ranges into history once per second
    if (millis() - lastHistMs >= 1000) {
        lastHistMs = millis();
        histX[histIdx % CAL_HIST] = maxX - minX;
        histY[histIdx % CAL_HIST] = maxY - minY;
        histIdx++;
    }

    if (elapsed > CAL_TIMEOUT_MS) {
        calAbort("timeout — spin incomplete or magnetic interference");
        return;
    }

    if (elapsed > CAL_MIN_MS && histIdx >= CAL_HIST) {
        float loX = histX[0], hiX = histX[0];
        float loY = histY[0], hiY = histY[0];
        for (int i = 1; i < CAL_HIST; i++) {
            if (histX[i] < loX) loX = histX[i];  if (histX[i] > hiX) hiX = histX[i];
            if (histY[i] < loY) loY = histY[i];  if (histY[i] > hiY) hiY = histY[i];
        }
        float growthX = hiX - loX;
        float growthY = hiY - loY;
        float rangeX  = maxX - minX;
        float rangeY  = maxY - minY;
        if (growthX < CAL_PLATEAU_UT && growthY < CAL_PLATEAU_UT
            && rangeX > CAL_MIN_RANGE && rangeY > CAL_MIN_RANGE) {
            calSuccess();
        }
    }
}

// Progress estimate for the app: clamp(max(rangeX,rangeY)/CAL_MIN_RANGE).
// Hits 100% once ranges are credible; plateau detection takes a bit longer.
static int calProgressPercent() {
    if (calState != CAL_COLLECTING) return calState == CAL_DONE ? 100 : 0;
    float rX = maxX - minX, rY = maxY - minY;
    float best = rX > rY ? rX : rY;
    int pct = (int)(best / CAL_MIN_RANGE * 100.0f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// ── HTTP ──────────────────────────────────────────────────────────────────
static void addCORS() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}
static void handleOptions() { addCORS(); server.send(204); }

static void handleStatus() {
    addCORS();
    String body = String("{\"ok\":true,\"v\":\"test_30\",\"ip\":\"") + boatIP
                + "\",\"calibrated\":" + (magCalibrated() ? "true" : "false") + "}";
    server.send(200, "application/json", body);
}

static void handleTelemetry() {
    addCORS();
    StaticJsonDocument<512> doc;
    doc["v"]                = "test_30";
    doc["session_id"]       = sessionId;
    doc["uptime"]           = millis() / 1000;
    doc["heap"]             = ESP.getFreeHeap();
    doc["mag_cal_state"]    = calStateName(calState);
    doc["mag_cal_progress"] = calProgressPercent();
    doc["mag_calibrated"]   = magCalibrated();
    doc["mag_cal_ts"]       = magCalTs;
    if (calState == CAL_FAILED) doc["mag_cal_fail"] = failReason;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", magOffX);       doc["mag_off_x"]    = buf;
    snprintf(buf, sizeof(buf), "%.2f", magOffY);       doc["mag_off_y"]    = buf;
    snprintf(buf, sizeof(buf), "%.2f", magOffZ);       doc["mag_off_z"]    = buf;
    snprintf(buf, sizeof(buf), "%.2f", magBaselineUT); doc["mag_baseline_uT"] = buf;
    snprintf(buf, sizeof(buf), "%.2f", liveMagUT);     doc["mag_uT"]       = buf;
    snprintf(buf, sizeof(buf), "%.1f", liveHeading);   doc["heading"]      = buf;
    char out[512];
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleCalStart() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    if (calState == CAL_COLLECTING) {
        server.send(200, "application/json", "{\"ok\":true,\"state\":\"collecting\",\"note\":\"already running\"}");
        return;
    }
    calBegin();
    server.send(200, "application/json", "{\"ok\":true,\"state\":\"collecting\"}");
}

static void handleCalAbort() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    if (calState != CAL_COLLECTING) {
        server.send(200, "application/json", "{\"ok\":true,\"state\":\"idle\",\"note\":\"not collecting\"}");
        if (calState != CAL_FAILED) calState = CAL_IDLE;
        return;
    }
    calAbort("operator aborted");
    calState = CAL_IDLE;
    server.send(200, "application/json", "{\"ok\":true,\"state\":\"idle\"}");
}

// ── Setup / Loop ──────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("test_30_mag_calibration");
    sessionId = esp_random();

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

    loadCalFromNVS();
    wifiSetup();

    server.on("/status",              HTTP_GET,     handleStatus);
    server.on("/status",              HTTP_OPTIONS, handleOptions);
    server.on("/telemetry",           HTTP_GET,     handleTelemetry);
    server.on("/telemetry",           HTTP_OPTIONS, handleOptions);
    server.on("/calibrate_mag/start", HTTP_POST,    handleCalStart);
    server.on("/calibrate_mag/start", HTTP_OPTIONS, handleOptions);
    server.on("/calibrate_mag/abort", HTTP_POST,    handleCalAbort);
    server.on("/calibrate_mag/abort", HTTP_OPTIONS, handleOptions);
    server.begin();
    if (boatIP.length()) Serial.printf("HTTP up at http://%s/\n", boatIP.c_str());

    Serial.printf("State: %s   calibrated=%s\n",
                  calStateName(calState), magCalibrated() ? "true" : "false");
    Serial.println("POST /calibrate_mag/start to begin.");
}

void loop() {
    server.handleClient();

    // Always refresh live IMU values for heading + magnitude readouts.
    // calTick() inside CAL_COLLECTING also reads the IMU, so guard the
    // outside path to avoid double-reads when collecting.
    if (calState == CAL_COLLECTING) {
        calTick();
        recomputeHeading();   // operator wants to see heading move during cal
    } else {
        if (readIMU()) recomputeHeading();
    }

    // 1 Hz Serial summary during active cal so the bench operator can see
    // progress without polling /telemetry.
    static unsigned long lastSerialMs = 0;
    if (calState == CAL_COLLECTING && millis() - lastSerialMs > 1000) {
        lastSerialMs = millis();
        Serial.printf("[CAL] %3d%% rX=%.1f rY=%.1f rZ=%.1f samples=%d\n",
                      calProgressPercent(),
                      maxX - minX, maxY - minY, maxZ - minZ, sampleCnt);
    }
}
