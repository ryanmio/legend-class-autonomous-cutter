/*
 * test_25_waypoint_heading.ino
 *
 * GPS waypoint → haversine bearing → proportional heading hold.
 * Extends test_24: everything is the same except the controller's target
 * heading now comes from the bearing to a GPS waypoint set via the app,
 * rather than a manually-typed number.
 *
 * Flow:
 *   App drops waypoint on map → POST /waypoint {"lat":…,"lon":…}
 *   → firmware computes bearing (haversine) from current GPS position
 *   → bearing becomes targetHeading for the proportional controller
 *   → rudder deflects to close heading error
 *
 * Bench test procedure:
 *   1. Get a GPS fix (outdoors or near a window).
 *   2. Drop a waypoint on the app map. Note the bearing shown on screen.
 *   3. Rotate the boat until the rudder goes close to neutral.
 *   4. Type 'W' — snapshot prints bearing, IMU heading, error.
 *   5. PASS if app bearing ≈ firmware bearing (≤1° difference) AND
 *      error when rudder was straight is < 20°.
 *
 * MOUNTING_OFFSET_DEG defaults to 0. The bench test will reveal the actual
 * offset (IMU chip Y axis vs. boat bow). Calibrate on first water test using
 * GPS course-over-ground, then hardcode it here.
 *
 * Controller priority:
 *   1. Waypoint set + GPS fix → haversine bearing is the target
 *   2. Manual heading (H command) → fallback if no GPS fix
 *   3. Neither → rudder at neutral
 *
 * PASS criteria:
 *   GATE 1 — GPS fix acquired, position valid
 *   GATE 2 — POST /waypoint received, firmware telemetry shows wp_bearing
 *   GATE 3 — App-shown bearing and firmware wp_bearing agree within 1°
 *   GATE 4 — Rotate boat until rudder straightens, type 'W': error < 20°
 *   GATE 5 — Move waypoint on app, wp_bearing updates in telemetry
 *
 * Serial commands:
 *   W        — waypoint snapshot (GPS pos, waypoint, bearing, heading, error, rudder)
 *   H <deg>  — set manual target heading fallback (e.g. H 180)
 *   K <val>  — set proportional gain (e.g. K 3.0)
 *   G        — heading gate check (same as test_24)
 *   D        — raw IMU diag
 *   S        — stability test (5 s)
 *   T        — tracking test (15 s, 360° rotation)
 *
 * HTTP endpoints:
 *   GET  /status    — {"ok":true,"v":"test_25","ip":"…"}
 *   GET  /telemetry — full sensor JSON incl. wp_bearing, wp_dist_m, heading_error
 *   POST /waypoint  — {"lat":37.1,"lon":-122.5}  or {"lat":null} to clear
 *   POST /heading   — {"target":180.0}  manual fallback target
 *   POST /led       — {"light":"nav"|"bridge"|"deck","state":bool}
 *
 * Hardware: identical to test_24 — no new wiring required.
 *   PCA9685    SDA=GPIO21  SCL=GPIO22  addr=0x40  servo rail=6V
 *   ICM-20948  SDA=GPIO21  SCL=GPIO22  addr=0x68  AD0=GND
 *   Rudder servo: PCA9685 ch2
 *   BN-220 GPS: RX=GPIO17 (white wire)  TX=GPIO4  9600 baud
 *
 * Libraries:
 *   "Adafruit PWM Servo Driver Library"
 *   "SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library"
 *   "TinyGPSPlus" by Mikal Hart
 *   "ArduinoJson" by Benoit Blanchon
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_PWMServoDriver.h>
#include "ICM_20948.h"
#include <TinyGPSPlus.h>
#include <math.h>
#include "secrets.h"

// ── Rudder limits (test_15) ───────────────────────────────────────────────────
static const uint8_t  RUDDER_PCA_CH  = 2;
static const uint16_t RUDDER_MIN_US  = 1330;
static const uint16_t RUDDER_MAX_US  = 1670;
static const uint16_t RUDDER_NEUTRAL = 1500;

// ── Heading hold controller ───────────────────────────────────────────────────
// targetHeading is set automatically from waypoint bearing when GPS fix is valid.
// Falls back to manualTarget when waypoint is cleared.
static float targetHeading = -1.0f;   // -1 = inactive
static float manualTarget  = -1.0f;
static float Kp            =  3.0f;   // µs per degree of error

// ── Waypoint state ────────────────────────────────────────────────────────────
static bool  wpSet    = false;
static float wpLat    = 0.0f;
static float wpLon    = 0.0f;
static float wpBearing = 0.0f;   // haversine bearing from current GPS to waypoint
static float wpDistM   = 0.0f;   // distance in metres

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

// ── Hardware objects ──────────────────────────────────────────────────────────
static Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
static ICM_20948_I2C  myICM;
static TinyGPSPlus    gps;
static HardwareSerial gpsSerial(2);
static WebServer      server(80);
static String         boatIP;

// ── State ─────────────────────────────────────────────────────────────────────
static bool navOn = false, bridgeOn = false, deckOn = false;
static uint16_t currentRudderUs = RUDDER_NEUTRAL;

static float fusedHeading      = 0;
static float livePitch         = 0;
static float liveRoll          = 0;
static float liveAccelMag      = 0;
static unsigned long lastImuUs = 0;
static bool  headingInitialised = false;

// ── Haversine bearing and distance ────────────────────────────────────────────
// Returns bearing in [0, 360) degrees, clockwise from north.
static float haversineBearing(float fromLat, float fromLon, float toLat, float toLon) {
    float φ1 = fromLat * DEG_TO_RAD;
    float φ2 = toLat   * DEG_TO_RAD;
    float Δλ = (toLon - fromLon) * DEG_TO_RAD;
    float y  = sinf(Δλ) * cosf(φ2);
    float x  = cosf(φ1) * sinf(φ2) - sinf(φ1) * cosf(φ2) * cosf(Δλ);
    float b  = atan2f(y, x) * RAD_TO_DEG;
    return fmodf(b + 360.0f, 360.0f);
}

static float haversineDistM(float fromLat, float fromLon, float toLat, float toLon) {
    const float R = 6371000.0f;
    float φ1 = fromLat * DEG_TO_RAD;
    float φ2 = toLat   * DEG_TO_RAD;
    float Δφ = (toLat  - fromLat) * DEG_TO_RAD;
    float Δλ = (toLon  - fromLon) * DEG_TO_RAD;
    float a  = sinf(Δφ / 2) * sinf(Δφ / 2)
             + cosf(φ1) * cosf(φ2) * sinf(Δλ / 2) * sinf(Δλ / 2);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static uint16_t usTicks(uint16_t us) {
    return (uint16_t)((us / 20000.0f) * 4096);
}

static void setRudder(uint16_t us) {
    if (us < RUDDER_MIN_US) us = RUDDER_MIN_US;
    if (us > RUDDER_MAX_US) us = RUDDER_MAX_US;
    currentRudderUs = us;
    pca.setPWM(RUDDER_PCA_CH, 0, usTicks(us));
}

static float shortestPathError(float target, float current) {
    float err = target - current;
    while (err >  180.0f) err -= 360.0f;
    while (err < -180.0f) err += 360.0f;
    return err;
}

// ── Update waypoint bearing from current GPS position ─────────────────────────
static void updateWaypointBearing() {
    if (!wpSet) return;
    bool hasFix = gps.location.isValid() && gps.location.age() < 5000;
    if (!hasFix) return;
    float lat = (float)gps.location.lat();
    float lon = (float)gps.location.lng();
    wpBearing  = haversineBearing(lat, lon, wpLat, wpLon);
    wpDistM    = haversineDistM(lat, lon, wpLat, wpLon);
    targetHeading = wpBearing;
}

// ── Proportional heading hold ─────────────────────────────────────────────────
static void applyController() {
    if (targetHeading < 0.0f || !headingInitialised) {
        setRudder(RUDDER_NEUTRAL);
        return;
    }
    float err   = shortestPathError(targetHeading, fusedHeading);
    float rawUs = RUDDER_NEUTRAL + Kp * err;
    setRudder((uint16_t)constrain((int)rawUs, RUDDER_MIN_US, RUDDER_MAX_US));
}

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

// ── IMU (identical to test_24) ────────────────────────────────────────────────
static void updateIMU() {
    if (!myICM.dataReady()) return;
    myICM.getAGMT();

    float ax = myICM.accX(), ay = myICM.accY(), az = myICM.accZ();
    float gx = myICM.gyrX(), gy = myICM.gyrY(), gz = myICM.gyrZ();
    float mx = myICM.magX(), my = myICM.magY(), mz = myICM.magZ();

    mx -= MAG_OFFSET_X;  my -= MAG_OFFSET_Y;  mz -= MAG_OFFSET_Z;

    float ar_x = ay, ar_y = az, ar_z = ax;
    float mr_x = my, mr_y = mz, mr_z = mx;

    liveRoll  = atan2f(ar_y, ar_z) * 180.0f / PI;
    livePitch = atan2f(-ar_x, sqrtf(ar_y*ar_y + ar_z*ar_z)) * 180.0f / PI;

    float rR = liveRoll * PI / 180.0f;
    float pR = livePitch * PI / 180.0f;
    float Bx = mr_x*cosf(pR) + mr_y*sinf(rR)*sinf(pR) + mr_z*cosf(rR)*sinf(pR);
    float By = mr_y*cosf(rR) - mr_z*sinf(rR);
    float magH = atan2f(-By, Bx) * 180.0f / PI;
    if (magH < 0) magH += 360.0f;

    liveAccelMag = sqrtf(ax*ax + ay*ay + az*az);

    unsigned long nowUs = micros();
    float dt = (lastImuUs == 0) ? 0.0f : (nowUs - lastImuUs) * 1e-6f;
    lastImuUs = nowUs;

    if (!headingInitialised) {
        fusedHeading = magH;
        headingInitialised = true;
    } else {
        float yawRate = (liveAccelMag > 100.0f)
                      ? (ax*gx + ay*gy + az*gz) / liveAccelMag
                      : 0.0f;
        float gyroH = fusedHeading + yawRate * dt;
        while (gyroH <   0.0f) gyroH += 360.0f;
        while (gyroH >= 360.0f) gyroH -= 360.0f;
        float diff = magH - gyroH;
        if (diff >  180.0f) diff -= 360.0f;
        if (diff < -180.0f) diff += 360.0f;
        fusedHeading = gyroH + (1.0f - ALPHA) * diff;
        if (fusedHeading <   0.0f) fusedHeading += 360.0f;
        if (fusedHeading >= 360.0f) fusedHeading -= 360.0f;
    }
}

// ── Serial test routines ──────────────────────────────────────────────────────
static void runWaypointSnapshot() {
    bool hasFix = gps.location.isValid() && gps.location.age() < 5000;
    Serial.println("WAYPOINT SNAPSHOT");
    if (!hasFix) {
        Serial.println("  GPS fix     : NO — move near a window and wait");
    } else {
        Serial.printf( "  GPS pos     : %.6f, %.6f  (%d sats)\n",
                       gps.location.lat(), gps.location.lng(), (int)gps.satellites.value());
    }
    if (!wpSet) {
        Serial.println("  Waypoint    : not set — drop one on the app map");
    } else {
        Serial.printf( "  Waypoint    : %.6f, %.6f\n", wpLat, wpLon);
        if (hasFix) {
            Serial.printf("  WP bearing  : %.1f deg (haversine)\n", wpBearing);
            Serial.printf("  WP distance : %.1f m\n", wpDistM);
            Serial.printf("  IMU heading : %.1f deg\n", fusedHeading);
            float err = shortestPathError(targetHeading, fusedHeading);
            Serial.printf("  Heading err : %+.1f deg\n", err);
            Serial.printf("  Rudder      : %d µs  (%+d from neutral)\n",
                          currentRudderUs, (int)currentRudderUs - RUDDER_NEUTRAL);
            bool pass = fabsf(err) < 20.0f;
            if (pass) Serial.println("  [GATE 4] PASS — heading error < 20 deg while rudder near straight");
            else      Serial.printf( "  [GATE 4] FAIL — error %.1f deg (need < 20). Rotate boat further.\n", fabsf(err));
        }
    }
}

static void runGateCheck() {
    if (targetHeading < 0.0f) {
        Serial.println("[GATE CHECK] No active target. Set waypoint or use H command.");
        return;
    }
    float err  = shortestPathError(targetHeading, fusedHeading);
    int16_t defl = (int16_t)currentRudderUs - (int16_t)RUDDER_NEUTRAL;
    bool dirOK   = (err > 0 && defl > 0) || (err < 0 && defl < 0) || (fabsf(err) < 3.0f);
    bool hasDefl = abs(defl) > 20;
    Serial.println("GATE CHECK SNAPSHOT");
    Serial.printf("  Target  : %.1f deg  IMU: %.1f deg  Error: %+.1f deg\n",
                  targetHeading, fusedHeading, err);
    Serial.printf("  Rudder  : %d µs  (%+d from neutral)\n", currentRudderUs, defl);
    if (hasDefl && dirOK)   Serial.println("  [GATE 3] PASS — correct direction");
    else if (!dirOK)        Serial.println("  [GATE 3] FAIL — wrong direction. Check mounting offset.");
    else                    Serial.println("  [GATE 3] FAIL — barely moved. Rotate boat further or raise Kp.");
}

static void runStabilityTest() {
    float minH = fusedHeading, maxH = fusedHeading;
    unsigned long start = millis();
    while (millis() - start < 5000) {
        while (gpsSerial.available()) gps.encode(gpsSerial.read());
        updateIMU(); updateWaypointBearing(); applyController();
        server.handleClient();
        if (fusedHeading < minH) minH = fusedHeading;
        if (fusedHeading > maxH) maxH = fusedHeading;
        delay(10);
    }
    float range = maxH - minH;
    if (range > 180.0f) range = 360.0f - range;
    Serial.printf("Range: %.1f deg  ");
    if (range <= 5.0f) Serial.println("[STABILITY] PASS");
    else               Serial.println("[STABILITY] FAIL");
}

static void runTrackingTest() {
    float prev = fusedHeading, start_h = fusedHeading, cum = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 15000) {
        while (gpsSerial.available()) gps.encode(gpsSerial.read());
        updateIMU(); updateWaypointBearing(); applyController();
        server.handleClient();
        float d = fusedHeading - prev;
        if (d >  180.0f) d -= 360.0f;
        if (d < -180.0f) d += 360.0f;
        cum += d; prev = fusedHeading;
        delay(10);
    }
    float ret = fabsf(fusedHeading - start_h);
    if (ret > 180.0f) ret = 360.0f - ret;
    Serial.printf("Start: %.1f  End: %.1f  Cumulative: %.1f  Return error: %.1f deg\n",
                  start_h, fusedHeading, cum, ret);
    if (fabsf(cum) >= 300.0f && ret <= 20.0f) Serial.println("[TRACKING] PASS");
    else if (fabsf(cum) < 300.0f)             Serial.println("[TRACKING] FAIL — spin further");
    else                                       Serial.println("[TRACKING] FAIL — did not return within 20 deg");
}

static void printDiag() {
    if (!myICM.dataReady()) { Serial.println("IMU not ready"); return; }
    myICM.getAGMT();
    Serial.printf("ax=%6.1f ay=%6.1f az=%6.1f mg\n", myICM.accX(), myICM.accY(), myICM.accZ());
    Serial.printf("mx=%6.2f my=%6.2f mz=%6.2f uT\n", myICM.magX(), myICM.magY(), myICM.magZ());
    Serial.printf("heading=%.1f  pitch=%.1f  roll=%.1f  target=%.1f  rudder=%d us\n",
                  fusedHeading, livePitch, liveRoll, targetHeading, currentRudderUs);
    if (wpSet) Serial.printf("wp_bearing=%.1f  wp_dist=%.1f m\n", wpBearing, wpDistM);
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
    server.send(200, "application/json",
        "{\"ok\":true,\"v\":\"test_25\",\"ip\":\"" + boatIP + "\"}");
}

static void handleTelemetry() {
    addCORS();
    StaticJsonDocument<640> doc;
    doc["v"]         = "test_25";
    doc["uptime"]    = millis() / 1000;
    doc["heap"]      = ESP.getFreeHeap();
    doc["nav_on"]    = navOn;
    doc["bridge_on"] = bridgeOn;
    doc["deck_on"]   = deckOn;
    doc["rudder_us"] = currentRudderUs;

    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f", fusedHeading); doc["heading"]   = buf;
    snprintf(buf, sizeof(buf), "%.1f", livePitch);    doc["pitch"]     = buf;
    snprintf(buf, sizeof(buf), "%.1f", liveRoll);     doc["roll"]      = buf;
    doc["accel_mag"] = (int)liveAccelMag;

    if (targetHeading >= 0.0f) {
        snprintf(buf, sizeof(buf), "%.1f", targetHeading);
        doc["heading_target"] = buf;
        snprintf(buf, sizeof(buf), "%+.1f", shortestPathError(targetHeading, fusedHeading));
        doc["heading_error"]  = buf;
    }

    if (wpSet) {
        snprintf(buf, sizeof(buf), "%.1f", wpBearing); doc["wp_bearing"] = buf;
        doc["wp_dist_m"] = (int)wpDistM;
    }

    bool hasFix = gps.location.isValid() && gps.location.age() < 5000;
    doc["gps_fix"] = hasFix;
    doc["sats"]    = (int)gps.satellites.value();
    if (hasFix) {
        snprintf(buf, sizeof(buf), "%.6f", gps.location.lat()); doc["lat"] = buf;
        snprintf(buf, sizeof(buf), "%.6f", gps.location.lng()); doc["lon"] = buf;
        snprintf(buf, sizeof(buf), "%.1f", gps.speed.knots());  doc["speed_kts"] = buf;
        snprintf(buf, sizeof(buf), "%.1f", gps.course.deg());   doc["course"]    = buf;
    }

    char out[640];
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleWaypoint() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    // {"lat":null,"lon":null} clears the waypoint.
    if (req["lat"].isNull() || req["lon"].isNull()) {
        wpSet = false;
        targetHeading = manualTarget;   // fall back to manual target (or -1 if none)
        server.send(200, "application/json", "{\"ok\":true,\"cleared\":true}");
        return;
    }

    wpLat = req["lat"].as<float>();
    wpLon = req["lon"].as<float>();
    wpSet = true;
    updateWaypointBearing();   // compute immediately from current GPS position
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleHeading() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<64> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }
    if (!req.containsKey("target")) { server.send(400, "text/plain", "Missing 'target'"); return; }

    float t = req["target"].as<float>();
    while (t <   0.0f) t += 360.0f;
    while (t >= 360.0f) t -= 360.0f;
    manualTarget = t;
    if (!wpSet) targetHeading = manualTarget;   // only override if no GPS waypoint active
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleLed() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    const char* light = req["light"] | "";
    bool on = req["state"] | false;
    uint8_t pin = 0; bool* state = nullptr;
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
    Serial.println("test_25_waypoint_heading");

    pinMode(PIN_NAV,    OUTPUT); digitalWrite(PIN_NAV,    LOW);
    pinMode(PIN_BRIDGE, OUTPUT); digitalWrite(PIN_BRIDGE, LOW);
    pinMode(PIN_DECK,   OUTPUT); digitalWrite(PIN_DECK,   LOW);

    Wire.begin(21, 22);
    Wire.setClock(400000);

    pca.begin();
    pca.setOscillatorFrequency(27000000);
    pca.setPWMFreq(50);
    Wire.beginTransmission(0x40);
    bool pcaOK = (Wire.endTransmission() == 0);

    bool imuOK = false;
    for (int i = 1; i <= 3; i++) {
        myICM.begin(Wire, 0);
        if (myICM.status == ICM_20948_Stat_Ok) { imuOK = true; break; }
        delay(500);
    }

    if (!pcaOK || !imuOK) {
        if (!pcaOK) Serial.println("[FAIL] PCA9685 not found at 0x40");
        if (!imuOK) Serial.println("[FAIL] ICM-20948 not found at 0x68");
        while (true) delay(1000);
    }
    Serial.println("[I2C] PCA9685 + ICM-20948 both ready");

    setRudder(RUDDER_NEUTRAL);

    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[GPS] UART2 RX=GPIO%d TX=GPIO%d\n", GPS_RX_PIN, GPS_TX_PIN);

    wifiSetup();

    server.on("/status",    HTTP_GET,     handleStatus);
    server.on("/status",    HTTP_OPTIONS, handleOptions);
    server.on("/telemetry", HTTP_GET,     handleTelemetry);
    server.on("/telemetry", HTTP_OPTIONS, handleOptions);
    server.on("/waypoint",  HTTP_POST,    handleWaypoint);
    server.on("/waypoint",  HTTP_OPTIONS, handleOptions);
    server.on("/heading",   HTTP_POST,    handleHeading);
    server.on("/heading",   HTTP_OPTIONS, handleOptions);
    server.on("/led",       HTTP_POST,    handleLed);
    server.on("/led",       HTTP_OPTIONS, handleOptions);
    server.begin();

    Serial.printf("[HTTP] port 80  IP: %s\n", boatIP.c_str());
    Serial.println("Ready. Waiting for GPS fix and waypoint from app.");
    Serial.println("  W        — waypoint snapshot (GATE 4 check)");
    Serial.println("  G        — heading gate check");
    Serial.println("  H <deg>  — manual target fallback");
    Serial.println("  K <val>  — set Kp gain");
    Serial.println("  D        — IMU diag");
    Serial.println("  S        — stability test");
    Serial.println("  T        — tracking test");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
static char    serialBuf[16];
static uint8_t serialLen = 0;

void loop() {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    updateIMU();
    updateWaypointBearing();
    applyController();
    server.handleClient();

    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialLen > 0) {
                serialBuf[serialLen] = '\0';
                char cmd = serialBuf[0];
                if      (cmd == 'W' || cmd == 'w') runWaypointSnapshot();
                else if (cmd == 'G' || cmd == 'g') runGateCheck();
                else if (cmd == 'S' || cmd == 's') runStabilityTest();
                else if (cmd == 'D' || cmd == 'd') printDiag();
                else if (cmd == 'T' || cmd == 't') runTrackingTest();
                else if (cmd == 'H' || cmd == 'h') {
                    float t = atof(serialBuf + 1);
                    while (t <   0.0f) t += 360.0f;
                    while (t >= 360.0f) t -= 360.0f;
                    manualTarget = t;
                    if (!wpSet) targetHeading = manualTarget;
                    Serial.printf("[TARGET] manual heading %.1f deg  (Kp=%.2f)\n", manualTarget, Kp);
                } else if (cmd == 'K' || cmd == 'k') {
                    float k = atof(serialBuf + 1);
                    if (k > 0.0f && k < 20.0f) { Kp = k; Serial.printf("[KP] %.2f\n", Kp); }
                    else Serial.println("[KP] out of range (0–20)");
                } else {
                    Serial.println("Commands: W  G  H<deg>  K<val>  D  S  T");
                }
                serialLen = 0;
            }
        } else if (serialLen < sizeof(serialBuf) - 1) {
            serialBuf[serialLen++] = c;
        }
    }
}
