/*
 * rc_loss_diag.ino — v2 (failsafe-guard verification)
 *
 * v1 confirmed the FS-iA10B holds-last on TX-off (frames keep streaming,
 * channels frozen). v2 verifies the workaround: SwD mapped to CH6 (idx 5)
 * with the receiver's failsafe value stored as ~2000 µs. Normal operation
 * leaves SwD up (~1000 µs); on TX-loss the receiver outputs ~2000 µs.
 *
 * No motor / servo / PCA9685 outputs at all — PCA is never initialized.
 * ESCs receive no PWM regardless. Safe to run with ESC battery connected.
 *
 * What it exposes:
 *   GET /channels  → JSON with all 10 iBUS channels, frame age/count, and
 *                    failsafe_active boolean.
 *
 * Serial prints (each fires ONCE per transition — no timer repeats):
 *   "[FAILSAFE DETECTED] ..."  — ch[5] > 1500 sustained 500 ms.
 *   "[FAILSAFE CLEARED]  ..."  — ch[5] back below 1500.
 *
 * Operator workflow:
 *   1. Flash. Power on. Note the IP printed at boot.
 *   2. TX on, SwD up. Verify ch[5] reads ~1000 via /channels.
 *   3. Turn TX off. Within ~500 ms, "[FAILSAFE DETECTED]" should print to
 *      Serial and /channels should show failsafe_active=true and ch[5]~2000.
 *   4. Turn TX back on (SwD still up). "[FAILSAFE CLEARED]" prints; ch[5]
 *      returns to ~1000; failsafe_active=false.
 *
 * If step 3 doesn't trigger, the TX failsafe config didn't take — check
 * the receiver was actually bound after the menu save, or that CH6
 * was the channel actually configured (not a sibling like CH7).
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "secrets.h"

static const uint8_t  IBUS_RX_PIN              = 16;
static const uint8_t  IBUS_IDX_FAILSAFE_GUARD  = 5;     // CH6 — SwD
static const uint16_t FAILSAFE_GUARD_THRESHOLD = 1500;  // mid-scale split
static const uint32_t FAILSAFE_DETECT_MS       = 500;   // hysteresis vs flicks

static HardwareSerial ibusSerial(1);
static WebServer      server(80);
static String         boatIP;

static uint8_t  ibusBuf[32], ibusIdx = 0;
static uint16_t ch[10] = {0};
static bool     ibusEverGood = false;
static uint32_t lastFrameMs  = 0;
static uint32_t framesTotal  = 0;
static uint32_t framesBad    = 0;

// Failsafe-guard detector state (transition-edge prints, no timer repeats).
static uint32_t guardAboveSinceMs = 0;
static bool     failsafeActive    = false;

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
    StaticJsonDocument<448> doc;
    doc["v"]                  = "rc_loss_diag";
    doc["uptime_s"]           = millis() / 1000;
    doc["frames_total"]       = framesTotal;
    doc["frames_bad"]         = framesBad;
    doc["ibus_ever_good"]     = ibusEverGood;
    doc["last_frame_age_ms"]  = ibusEverGood ? (millis() - lastFrameMs) : 0;
    doc["failsafe_active"]    = failsafeActive;
    doc["guard_us"]           = ch[IBUS_IDX_FAILSAFE_GUARD];
    JsonArray a = doc.createNestedArray("ch");
    for (int i = 0; i < 10; i++) a.add(ch[i]);
    char out[448];
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// Watch ch[5] (SwD failsafe guard). Rising edge sustained → print FAILSAFE
// DETECTED once. Falling edge → print FAILSAFE CLEARED once. Pre-RC state
// (no frames yet) is treated as not-failsafe.
static void updateFailsafeGuard() {
    if (!ibusEverGood) { guardAboveSinceMs = 0; failsafeActive = false; return; }
    uint16_t guard = ch[IBUS_IDX_FAILSAFE_GUARD];
    if (guard > FAILSAFE_GUARD_THRESHOLD) {
        if (guardAboveSinceMs == 0) guardAboveSinceMs = millis();
        if (!failsafeActive && (millis() - guardAboveSinceMs) >= FAILSAFE_DETECT_MS) {
            failsafeActive = true;
            Serial.printf("[FAILSAFE DETECTED] ch[5]=%u sustained %lu ms\n",
                guard, millis() - guardAboveSinceMs);
        }
    } else {
        guardAboveSinceMs = 0;
        if (failsafeActive) {
            failsafeActive = false;
            Serial.printf("[FAILSAFE CLEARED] ch[5]=%u\n", guard);
        }
    }
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
    updateFailsafeGuard();
    server.handleClient();
}
