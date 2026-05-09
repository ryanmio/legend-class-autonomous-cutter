/*
 * test_24_servo_sensors.ino
 *
 * Integration test: proportional heading hold on the bench.
 * PCA9685 rudder servo responds to IMU heading error in real time.
 *
 * Set a target heading, then physically rotate the boat. The rudder
 * deflects to correct the error — exactly how it will behave under sail.
 * This proves: (1) PCA9685 and ICM-20948 coexist on the shared I2C bus,
 * (2) the proportional controller drives the rudder in the right direction,
 * (3) rudder returns toward neutral as the boat is rotated back on target.
 *
 * Controller:
 *   error      = shortest-path (target_heading - fused_heading)
 *   rudder_us  = NEUTRAL + Kp * error
 *   clamped to [RUDDER_MIN_US, RUDDER_MAX_US]
 *
 * Rudder limits from test_15 (2026-05-03):
 *   RUDDER_MIN_US  1330  (left / port)
 *   RUDDER_MAX_US  1670  (right / starboard)
 *   RUDDER_NEUTRAL 1500
 *   PCA9685 channel 2
 *
 * Starting Kp = 3.0 µs/deg → full deflection at ~57° error. Tune with 'K'.
 *
 * Mag hard-iron offsets from test_22 run 2 (2026-05-08):
 *   MAG_OFFSET_X  -38.62 µT
 *   MAG_OFFSET_Y    1.65 µT
 *   MAG_OFFSET_Z  -14.85 µT
 *
 * PASS criteria:
 *   GATE 1 — PCA9685 (0x40) AND ICM-20948 (0x68) both init on shared I2C bus
 *   GATE 2 — WiFi connects, IP shown
 *   GATE 3 — Rotate boat ~30° off target, type 'G': rudder deflected in
 *             correct direction AND magnitude > 20 µs from neutral
 *   GATE 4 — Return boat near target, type 'G': rudder within 30 µs of neutral
 *   GATE 5 — App /telemetry shows heading_target, heading_error, rudder_us
 *
 * Serial commands:
 *   H <deg>  — set target heading, e.g. "H 180"
 *   K <val>  — set proportional gain, e.g. "K 3.0"
 *   G        — gate check: print heading/error/rudder PASS/FAIL snapshot
 *   D        — raw IMU accel/mag/gyro
 *   S        — stability test (5 s, heading variance)
 *   T        — tracking test (15 s, spin one full circle)
 *
 * HTTP endpoints:
 *   GET  /status    — {"ok":true,"v":"test_24","ip":"..."}
 *   GET  /telemetry — full sensor JSON incl. heading_target, heading_error, rudder_us
 *   POST /led       — {"light":"nav"|"bridge"|"deck","state":bool}
 *   POST /heading   — {"target":180.0}  sets target heading
 *
 * Hardware:
 *   PCA9685    SDA=GPIO21  SCL=GPIO22  addr=0x40  servo rail=6V
 *   ICM-20948  SDA=GPIO21  SCL=GPIO22  addr=0x68  AD0=GND
 *   Rudder servo: PCA9685 ch2
 *   BN-220 GPS: RX=GPIO17 (white wire)  TX=GPIO4  9600 baud
 *   LEDs: GPIO18=nav  GPIO19=bridge  GPIO23=deck
 *
 * Libraries:
 *   "Adafruit PWM Servo Driver Library"
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
#include <Adafruit_PWMServoDriver.h>
#include "ICM_20948.h"
#include <TinyGPSPlus.h>
#include "secrets.h"

// ── Rudder limits (test_15 2026-05-03) ───────────────────────────────────────
static const uint8_t  RUDDER_PCA_CH  = 2;
static const uint16_t RUDDER_MIN_US  = 1330;
static const uint16_t RUDDER_MAX_US  = 1670;
static const uint16_t RUDDER_NEUTRAL = 1500;

// ── Heading hold controller ───────────────────────────────────────────────────
static float targetHeading = -1.0f;   // -1 = not set (rudder stays at neutral)
static float Kp            =  3.0f;   // µs per degree of heading error

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

// Shortest-path angular difference: result in [-180, +180]
static float headingError(float target, float current) {
    float err = target - current;
    while (err >  180.0f) err -= 360.0f;
    while (err < -180.0f) err += 360.0f;
    return err;
}

// ── Heading hold — called every loop ─────────────────────────────────────────
static void applyController() {
    if (targetHeading < 0.0f || !headingInitialised) {
        setRudder(RUDDER_NEUTRAL);
        return;
    }
    float err    = headingError(targetHeading, fusedHeading);
    float rawUs  = RUDDER_NEUTRAL + Kp * err;
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

// ── IMU ───────────────────────────────────────────────────────────────────────
static void updateIMU() {
    if (!myICM.dataReady()) return;
    myICM.getAGMT();

    float ax = myICM.accX(), ay = myICM.accY(), az = myICM.accZ();
    float gx = myICM.gyrX(), gy = myICM.gyrY(), gz = myICM.gyrZ();
    float mx = myICM.magX(), my = myICM.magY(), mz = myICM.magZ();

    mx -= MAG_OFFSET_X;
    my -= MAG_OFFSET_Y;
    mz -= MAG_OFFSET_Z;

    // IMU mounted on vertical wall, X axis up (confirmed test_23).
    float ar_x = ay, ar_y = az, ar_z = ax;
    float mr_x = -my, mr_y = mz, mr_z = -mx;  // chip Y=stern(negate), chip X=up(negate for tilt formula)

    liveRoll  = atan2f(ar_y, ar_z) * 180.0f / PI;
    livePitch = atan2f(-ar_x, sqrtf(ar_y*ar_y + ar_z*ar_z)) * 180.0f / PI;

    float rR = liveRoll  * PI / 180.0f;
    float pR = livePitch * PI / 180.0f;
    float Bx = mr_x*cosf(pR) + mr_y*sinf(rR)*sinf(pR) + mr_z*cosf(rR)*sinf(pR);
    float By = mr_y*cosf(rR) - mr_z*sinf(rR);
    float magHeading = atan2f(-By, Bx) * 180.0f / PI;
    if (magHeading < 0) magHeading += 360.0f;

    liveAccelMag = sqrtf(ax*ax + ay*ay + az*az);

    unsigned long nowUs = micros();
    float dt = (lastImuUs == 0) ? 0.0f : (nowUs - lastImuUs) * 1e-6f;
    lastImuUs = nowUs;

    if (!headingInitialised) {
        fusedHeading      = magHeading;
        headingInitialised = true;
    } else {
        float aMag    = liveAccelMag;
        float yawRate = (aMag > 100.0f)
                      ? (ax*gx + ay*gy + az*gz) / aMag
                      : 0.0f;
        float gyroHeading = fusedHeading + yawRate * dt;
        while (gyroHeading <   0.0f) gyroHeading += 360.0f;
        while (gyroHeading >= 360.0f) gyroHeading -= 360.0f;

        float diff = magHeading - gyroHeading;
        if (diff >  180.0f) diff -= 360.0f;
        if (diff < -180.0f) diff += 360.0f;
        fusedHeading = gyroHeading + (1.0f - ALPHA) * diff;
        if (fusedHeading <   0.0f) fusedHeading += 360.0f;
        if (fusedHeading >= 360.0f) fusedHeading -= 360.0f;
    }
}

// ── Serial test routines ──────────────────────────────────────────────────────

// GATE CHECK — snapshot of current heading error vs rudder position.
// Run this after manually rotating the boat to an off-target bearing.
// Gate 3: boat ~30° off target → rudder deflected in correct direction, >20 µs from neutral
// Gate 4: boat back near target → rudder within 30 µs of neutral
static void runGateCheck() {
    if (targetHeading < 0.0f) {
        Serial.println("[GATE CHECK] FAIL — no target heading set. Use 'H <deg>' first.");
        return;
    }
    float err    = headingError(targetHeading, fusedHeading);
    int16_t defl = (int16_t)currentRudderUs - (int16_t)RUDDER_NEUTRAL;

    Serial.println("GATE CHECK SNAPSHOT");
    Serial.printf("  Target heading  : %.1f deg\n", targetHeading);
    Serial.printf("  Fused heading   : %.1f deg\n", fusedHeading);
    Serial.printf("  Heading error   : %+.1f deg\n", err);
    Serial.printf("  Rudder          : %d µs  (%+d from neutral)\n", currentRudderUs, defl);
    Serial.printf("  Kp              : %.2f µs/deg\n", Kp);

    // Direction correct if error and deflection have the same sign
    bool dirOK  = (err > 0 && defl > 0) || (err < 0 && defl < 0) || (fabsf(err) < 3.0f);
    bool hasDefl = abs(defl) > 20;
    bool nearTarget = abs(defl) <= 30;

    if (hasDefl && dirOK)
        Serial.println("  [GATE 3] PASS — rudder deflected in correct direction");
    else if (!dirOK)
        Serial.println("  [GATE 3] FAIL — rudder deflected in WRONG direction. Check mounting offset.");
    else
        Serial.println("  [GATE 3] FAIL — rudder barely moved (<20 µs). Increase Kp or rotate boat further.");

    if (nearTarget)
        Serial.println("  [GATE 4] PASS — rudder near neutral (boat near target heading)");
    else
        Serial.println("  [GATE 4] INFO  — boat still off target (expected if running Gate 3 check)");
}

static void runStabilityTest() {
    float minH = fusedHeading, maxH = fusedHeading;
    unsigned long start = millis();
    while (millis() - start < 5000) {
        while (gpsSerial.available()) gps.encode(gpsSerial.read());
        updateIMU();
        applyController();
        server.handleClient();
        if (fusedHeading < minH) minH = fusedHeading;
        if (fusedHeading > maxH) maxH = fusedHeading;
        delay(10);
    }
    float range = maxH - minH;
    if (range > 180.0f) range = 360.0f - range;
    Serial.printf("Min: %.1f°  Max: %.1f°  Range: %.1f°  ", minH, maxH, range);
    if (range <= 5.0f) Serial.println("[STABILITY] PASS");
    else               Serial.println("[STABILITY] FAIL — drift > 5 deg");
}

static void runTrackingTest() {
    float prevHeading        = fusedHeading;
    float startHeading       = fusedHeading;
    float cumulativeRotation = 0;
    unsigned long start      = millis();
    while (millis() - start < 15000) {
        while (gpsSerial.available()) gps.encode(gpsSerial.read());
        updateIMU();
        applyController();
        server.handleClient();
        float delta = fusedHeading - prevHeading;
        if (delta >  180.0f) delta -= 360.0f;
        if (delta < -180.0f) delta += 360.0f;
        cumulativeRotation += delta;
        prevHeading = fusedHeading;
        delay(10);
    }
    float returnError = fabsf(fusedHeading - startHeading);
    if (returnError > 180.0f) returnError = 360.0f - returnError;
    float absCum = fabsf(cumulativeRotation);
    Serial.printf("Start: %.1f°  End: %.1f°  Cumulative: %.1f°  Return error: %.1f°\n",
                  startHeading, fusedHeading, cumulativeRotation, returnError);
    if (absCum >= 300.0f && returnError <= 20.0f)
        Serial.println("[TRACKING] PASS");
    else if (absCum < 300.0f)
        Serial.printf("[TRACKING] FAIL — only %.1f° cumulative (need 300+)\n", absCum);
    else
        Serial.println("[TRACKING] FAIL — did not return within 20°");
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
    Serial.printf("     heading=%.1f°  pitch=%.1f°  roll=%.1f°\n",
                  fusedHeading, livePitch, liveRoll);
    if (targetHeading >= 0.0f)
        Serial.printf("     target=%.1f°  error=%+.1f°  rudder=%d µs\n",
                      targetHeading, headingError(targetHeading, fusedHeading), currentRudderUs);
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
    String body = "{\"ok\":true,\"v\":\"test_24\",\"ip\":\"" + boatIP + "\"}";
    server.send(200, "application/json", body);
}

static void handleTelemetry() {
    addCORS();
    StaticJsonDocument<512> doc;
    doc["v"]          = "test_24";
    doc["uptime"]     = millis() / 1000;
    doc["heap"]       = ESP.getFreeHeap();
    doc["nav_on"]     = navOn;
    doc["bridge_on"]  = bridgeOn;
    doc["deck_on"]    = deckOn;

    // Controller state
    doc["rudder_us"] = currentRudderUs;
    if (targetHeading >= 0.0f) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", targetHeading);
        doc["heading_target"] = buf;
        snprintf(buf, sizeof(buf), "%+.1f", headingError(targetHeading, fusedHeading));
        doc["heading_error"]  = buf;
    }

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

static void handleHeading() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<64> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }
    if (!req.containsKey("target")) { server.send(400, "text/plain", "Missing 'target'"); return; }

    float t = req["target"];
    while (t <   0.0f) t += 360.0f;
    while (t >= 360.0f) t -= 360.0f;
    targetHeading = t;
    server.send(200, "application/json", "{\"ok\":true}");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("test_24_servo_sensors");

    pinMode(PIN_NAV,    OUTPUT); digitalWrite(PIN_NAV,    LOW);
    pinMode(PIN_BRIDGE, OUTPUT); digitalWrite(PIN_BRIDGE, LOW);
    pinMode(PIN_DECK,   OUTPUT); digitalWrite(PIN_DECK,   LOW);

    Wire.begin(21, 22);
    Wire.setClock(400000);

    // ── GATE 1: both devices on shared I2C bus ───────────────────────────────
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

    if (pcaOK && imuOK) {
        Serial.println("[GATE 1] PASS — PCA9685 (0x40) and ICM-20948 (0x68) both on I2C bus");
    } else {
        if (!pcaOK) Serial.println("[GATE 1] FAIL — PCA9685 not found at 0x40");
        if (!imuOK) Serial.println("[GATE 1] FAIL — ICM-20948 not found at 0x68");
        while (true) delay(1000);
    }

    setRudder(RUDDER_NEUTRAL);
    Serial.printf("[RUDDER] ch2 neutral (%d µs)\n", RUDDER_NEUTRAL);

    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[GPS] UART2 RX=GPIO%d TX=GPIO%d @ %d baud\n",
                  GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

    wifiSetup();
    Serial.println("[GATE 2] PASS candidate — see IP above");

    server.on("/status",    HTTP_GET,     handleStatus);
    server.on("/status",    HTTP_OPTIONS, handleOptions);
    server.on("/telemetry", HTTP_GET,     handleTelemetry);
    server.on("/telemetry", HTTP_OPTIONS, handleOptions);
    server.on("/led",       HTTP_POST,    handleLed);
    server.on("/led",       HTTP_OPTIONS, handleOptions);
    server.on("/heading",   HTTP_POST,    handleHeading);
    server.on("/heading",   HTTP_OPTIONS, handleOptions);
    server.begin();
    Serial.printf("[HTTP] port 80  /status  /telemetry  /led  /heading  (IP: %s)\n", boatIP.c_str());
    Serial.println("Ready. Controller inactive until target is set.");
    Serial.println("  H <deg>  — set target heading (e.g. H 180)");
    Serial.println("  K <val>  — set Kp gain (e.g. K 3.0)");
    Serial.println("  G        — gate check snapshot");
    Serial.println("  D        — raw IMU diag");
    Serial.println("  S        — stability test (5 s)");
    Serial.println("  T        — tracking test (15 s, spin one circle)");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
static char    serialBuf[16];
static uint8_t serialLen = 0;

void loop() {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    updateIMU();
    applyController();
    server.handleClient();

    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialLen > 0) {
                serialBuf[serialLen] = '\0';
                char cmd = serialBuf[0];
                if (cmd == 'H' || cmd == 'h') {
                    float t = atof(serialBuf + 1);
                    while (t <   0.0f) t += 360.0f;
                    while (t >= 360.0f) t -= 360.0f;
                    targetHeading = t;
                    Serial.printf("[TARGET] heading set to %.1f deg  (Kp=%.2f)\n", targetHeading, Kp);
                } else if (cmd == 'K' || cmd == 'k') {
                    float k = atof(serialBuf + 1);
                    if (k > 0.0f && k < 20.0f) {
                        Kp = k;
                        Serial.printf("[KP] set to %.2f µs/deg\n", Kp);
                    } else {
                        Serial.println("[KP] out of range (0–20). Not changed.");
                    }
                } else if (cmd == 'G' || cmd == 'g') {
                    runGateCheck();
                } else if (cmd == 'S' || cmd == 's') {
                    runStabilityTest();
                } else if (cmd == 'D' || cmd == 'd') {
                    printDiag();
                } else if (cmd == 'T' || cmd == 't') {
                    runTrackingTest();
                } else {
                    Serial.println("Commands: H<deg>  K<val>  G  S  D  T");
                }
                serialLen = 0;
            }
        } else if (serialLen < sizeof(serialBuf) - 1) {
            serialBuf[serialLen++] = c;
        }
    }
}
