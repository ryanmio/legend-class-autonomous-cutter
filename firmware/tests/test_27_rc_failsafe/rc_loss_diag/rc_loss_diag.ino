/*
 * rc_loss_diag.ino
 *
 * Diagnostic for "what does the FS-iA10B do when the TX is turned off?"
 * test_27 v1's no-frame failsafe never tripped on TX power-off, so the
 * receiver must be sending something. This sketch shows what.
 *
 * No motor / servo / PCA9685 outputs at all — PCA is never initialized,
 * so ESCs never receive PWM. You can unplug the ESC battery for extra
 * peace of mind; the diagnostic still runs.
 *
 * What it exposes:
 *   GET /channels  → JSON with all 10 iBUS channels, frame age, frame count.
 *
 * Operator workflow:
 *   1. Flash. Power on. Wait for "[WiFi] OK <ip>" line.
 *   2. TX on, sticks/switches in any deliberate position.
 *   3. From a browser or curl: GET http://<ip>/channels  → record values.
 *   4. Turn TX off. Wait 5 s.
 *   5. GET http://<ip>/channels again → record values.
 *   6. Compare.
 *
 * Three plausible patterns:
 *   - HOLD-LAST: channels identical between requests, frame_age_ms ~16 in
 *     both. Receiver is repeating the last good frame indefinitely.
 *   - PRESETS: some/all channels jump to specific values; frame_age_ms ~16.
 *     Receiver is sending TX-loss preset values (configurable on TX).
 *   - SILENT: second request shows frame_age_ms >> 5000, or frames_total
 *     unchanged from request 1. Receiver stopped iBUS entirely.
 *
 * The pattern determines the failsafe-detection strategy in test_27 v2.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "secrets.h"

static const uint8_t  IBUS_RX_PIN = 16;

static HardwareSerial ibusSerial(1);
static WebServer      server(80);
static String         boatIP;

static uint8_t  ibusBuf[32], ibusIdx = 0;
static uint16_t ch[10] = {0};
static bool     ibusEverGood = false;
static uint32_t lastFrameMs  = 0;
static uint32_t framesTotal  = 0;
static uint32_t framesBad    = 0;

// ── iBUS parsing (same shape as test_26 / test_27) ──────────────────────────
static bool parseIbus() {
    if (ibusBuf[0] != 0x20 || ibusBuf[1] != 0x40) { framesBad++; return false; }
    uint16_t sum = 0xFFFF;
    for (int i = 0; i < 30; i++) sum -= ibusBuf[i];
    uint16_t rx = ibusBuf[30] | (ibusBuf[31] << 8);
    if (sum != rx) { framesBad++; return false; }
    for (int i = 0; i < 10; i++)
        ch[i] = ibusBuf[2 + i*2] | (ibusBuf[3 + i*2] << 8);
    lastFrameMs = millis();
    framesTotal++;
    if (!ibusEverGood) ibusEverGood = true;
    return true;
}
static void readIbus() {
    while (ibusSerial.available()) {
        uint8_t b = ibusSerial.read();
        if (ibusIdx == 0 && b != 0x20) continue;
        ibusBuf[ibusIdx++] = b;
        if (ibusIdx == 32) { parseIbus(); ibusIdx = 0; }
    }
}

// ── WiFi (copied from test_27) ──────────────────────────────────────────────
static bool tryConnect(const char* ssid, const char* pass, int timeoutSecs) {
    WiFi.begin(ssid, pass);
    Serial.printf("[WiFi] Trying %s", ssid);
    for (int i = 0; i < timeoutSecs * 2; i++) {
        if (WiFi.status() == WL_CONNECTED) {
            boatIP = WiFi.localIP().toString();
            Serial.printf(" OK %s\n", boatIP.c_str());
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
    Serial.printf("[WiFi] AP fallback IP: %s\n", boatIP.c_str());
}

// ── HTTP ────────────────────────────────────────────────────────────────────
static void addCORS() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
}
static void handleOptions() { addCORS(); server.send(204); }

static void handleChannels() {
    addCORS();
    StaticJsonDocument<384> doc;
    doc["v"]                  = "rc_loss_diag";
    doc["uptime_s"]           = millis() / 1000;
    doc["frames_total"]       = framesTotal;
    doc["frames_bad"]         = framesBad;
    doc["ibus_ever_good"]     = ibusEverGood;
    doc["last_frame_age_ms"]  = ibusEverGood ? (millis() - lastFrameMs) : 0;
    JsonArray a = doc.createNestedArray("ch");
    for (int i = 0; i < 10; i++) a.add(ch[i]);
    char out[384];
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// ── Setup / loop ────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("rc_loss_diag  (NO motor outputs — PCA9685 not initialized)");

    ibusSerial.setRxBufferSize(1024);
    ibusSerial.begin(115200, SERIAL_8N1, IBUS_RX_PIN, -1);
    Serial.printf("iBUS on GPIO%d.\n", IBUS_RX_PIN);

    wifiSetup();

    server.on("/channels", HTTP_GET,     handleChannels);
    server.on("/channels", HTTP_OPTIONS, handleOptions);
    server.begin();
    Serial.printf("Ready. GET http://%s/channels\n", boatIP.c_str());
}

void loop() {
    readIbus();
    server.handleClient();
}
