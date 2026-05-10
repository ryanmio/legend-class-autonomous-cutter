/*
 * freeze_diag.ino
 *
 * Definitive diagnostic for the test_17 / test_26 "channels frozen" failsafe
 * trip. Strips everything except the iBUS reader so nothing else (IMU, PCA,
 * WiFi) can confound the measurement.
 *
 * Multi-phase, operator-paced. Sketch is silent during each phase. Output only
 * happens at boot (instructions), between phases (next-step prompt), and at
 * the end (one results block).
 *
 * Phases:
 *   1. Sticks at REST           (10 s) — proves baseline behavior
 *   2. Wiggle RUDDER stick CH1  (10 s) — proves channels change on movement
 *   3. Wiggle THROTTLE stick CH3(10 s) — same on a different channel
 *   4. Sticks at REST again     (10 s) — confirms phase 1 wasn't a fluke
 *
 * Final report shows per-phase: good/bad frame counts, max gap between good
 * frames (catches brownouts), freeze-trip count (matches test_17 detector at
 * 500 ms), and which channels changed during the phase.
 *
 * Hardware: only the iBUS divider on GPIO 16. Nothing else needed.
 */

#include <Arduino.h>

static const uint8_t  IBUS_RX_PIN        = 16;
static const uint32_t FREEZE_THRESH_MS   = 500;     // matches test_17
static const uint32_t PHASE_DURATION_MS  = 10000;   // 10 s per phase
static const uint8_t  NUM_PHASES         = 4;

static const char* PHASE_NAMES[NUM_PHASES] = {
    "Phase 1: REST (do not touch sticks)",
    "Phase 2: WIGGLE RUDDER (CH1, right-stick H)",
    "Phase 3: WIGGLE THROTTLE (CH3, left-stick V)",
    "Phase 4: REST again (do not touch sticks)",
};

static HardwareSerial ibusSerial(1);

// ── Frame parser ────────────────────────────────────────────────────────────
static uint8_t  ibusBuf[32];
static uint8_t  ibusIdx = 0;
static uint16_t ch[10] = {0};
static uint16_t prevCh[10];
static bool     prevValid = false;
static uint32_t lastChangeMs = 0;
static uint32_t lastGoodMs   = 0;
static bool     freezeStateActive = false;

// ── Per-phase stats ─────────────────────────────────────────────────────────
struct PhaseStats {
    uint32_t good;
    uint32_t bad;
    uint32_t maxGapMs;
    uint32_t freezeTrips;
    bool     chChanged[10];
    uint16_t chFinal[10];
};
static PhaseStats stats[NUM_PHASES] = {};
static int activePhase = -1;     // -1 = idle/waiting for command

static void resetPhaseStats(int p) {
    stats[p].good = 0;
    stats[p].bad  = 0;
    stats[p].maxGapMs = 0;
    stats[p].freezeTrips = 0;
    for (int i = 0; i < 10; i++) stats[p].chChanged[i] = false;
}

// ── Parse one iBUS frame from the buffer ────────────────────────────────────
static void parseFrame() {
    if (ibusBuf[0] != 0x20 || ibusBuf[1] != 0x40) {
        if (activePhase >= 0) stats[activePhase].bad++;
        return;
    }
    uint16_t sum = 0xFFFF;
    for (int i = 0; i < 30; i++) sum -= ibusBuf[i];
    uint16_t rx = ibusBuf[30] | (ibusBuf[31] << 8);
    if (sum != rx) {
        if (activePhase >= 0) stats[activePhase].bad++;
        return;
    }
    bool anyDiff = false;
    for (int i = 0; i < 10; i++) {
        uint16_t v = ibusBuf[2 + i*2] | (ibusBuf[3 + i*2] << 8);
        if (prevValid && v != prevCh[i]) {
            anyDiff = true;
            if (activePhase >= 0) stats[activePhase].chChanged[i] = true;
        }
        prevCh[i] = v;
        ch[i] = v;
    }
    prevValid = true;

    uint32_t now = millis();
    if (activePhase >= 0) {
        stats[activePhase].good++;
        if (lastGoodMs > 0) {
            uint32_t gap = now - lastGoodMs;
            if (gap > stats[activePhase].maxGapMs) stats[activePhase].maxGapMs = gap;
        }
    }
    lastGoodMs = now;

    if (anyDiff) {
        lastChangeMs = now;
        freezeStateActive = false;
    }
}

static void readIbus() {
    while (ibusSerial.available()) {
        uint8_t b = ibusSerial.read();
        if (ibusIdx == 0 && b != 0x20) continue;
        ibusBuf[ibusIdx++] = b;
        if (ibusIdx == 32) { parseFrame(); ibusIdx = 0; }
    }
}

static void tickFreezeCheck() {
    if (!prevValid || activePhase < 0) return;
    if (millis() - lastChangeMs > FREEZE_THRESH_MS) {
        if (!freezeStateActive) {
            freezeStateActive = true;
            stats[activePhase].freezeTrips++;
        }
    }
}

// ── Drain serial input down to the latest non-whitespace char ───────────────
static char readCommand() {
    char last = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c > ' ') last = c;
    }
    return last;
}

static void waitForChar(char wanted) {
    while (true) {
        readIbus();           // keep parser sync alive while waiting
        char c = readCommand();
        if (c == wanted || c == (wanted ^ 0x20)) return;   // accept either case
    }
}

// ── Run one phase silently for PHASE_DURATION_MS ────────────────────────────
static void runPhase(int p) {
    activePhase = p;
    resetPhaseStats(p);
    // Reset gap/freeze tracking so the inter-phase pause doesn't pollute stats.
    lastGoodMs   = millis();
    lastChangeMs = millis();
    freezeStateActive = false;

    uint32_t startMs = millis();
    while (millis() - startMs < PHASE_DURATION_MS) {
        readIbus();
        tickFreezeCheck();
    }
    for (int i = 0; i < 10; i++) stats[p].chFinal[i] = ch[i];
    activePhase = -1;
}

// ── Print final results ─────────────────────────────────────────────────────
static void printResults() {
    Serial.println();
    Serial.println("============== RESULTS ==============");
    for (int p = 0; p < NUM_PHASES; p++) {
        Serial.println();
        Serial.println(PHASE_NAMES[p]);
        Serial.printf("  frames good=%lu  bad=%lu  max_gap=%lums  freezeTrips=%lu\n",
            stats[p].good, stats[p].bad, stats[p].maxGapMs, stats[p].freezeTrips);
        Serial.print  ("  changed:");
        for (int i = 0; i < 10; i++) {
            Serial.printf(" ch%d:%c", i + 1, stats[p].chChanged[i] ? 'Y' : '.');
        }
        Serial.println();
        Serial.print  ("  final  :");
        for (int i = 0; i < 10; i++) {
            Serial.printf(" %u", stats[p].chFinal[i]);
        }
        Serial.println();
    }
    Serial.println();
    Serial.println("====== INTERPRETATION KEY ======");
    Serial.println("  freezeTrips > 0 in REST   = expected if RX has no jitter on idle sticks");
    Serial.println("  freezeTrips > 0 in WIGGLE = real bug (frame drops or RX failsafe behavior)");
    Serial.println("  changed Y on wiggled ch   = receiver IS reporting movement");
    Serial.println("  max_gap > 50 ms           = brief receiver brownout / RF gap");
    Serial.println("  bad frames spike          = link noise during that phase");
    Serial.println();
    Serial.println("Done. Reboot to re-run.");
}

// ── Setup runs the whole sketch start-to-finish ─────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("freeze_diag — definitive iBUS freeze-failsafe diagnostic");
    Serial.println();
    Serial.println("Each phase runs silently for 10 s. You'll be prompted between phases.");
    Serial.println("Turn TX on, throttle stick at the bottom.");
    Serial.println();
    Serial.println("Type 1 + ENTER to start Phase 1 (sticks at REST).");

    ibusSerial.setRxBufferSize(1024);
    ibusSerial.begin(115200, SERIAL_8N1, IBUS_RX_PIN, -1);

    waitForChar('1');
    runPhase(0);
    Serial.println("Phase 1 done. Type c + ENTER, then start WIGGLING RUDDER (CH1) for 10 s.");

    waitForChar('c');
    runPhase(1);
    Serial.println("Phase 2 done. Type c + ENTER, then start WIGGLING THROTTLE (CH3) for 10 s.");

    waitForChar('c');
    runPhase(2);
    Serial.println("Phase 3 done. Type c + ENTER, then leave sticks at REST for 10 s.");

    waitForChar('c');
    runPhase(3);

    printResults();
}

void loop() {}
