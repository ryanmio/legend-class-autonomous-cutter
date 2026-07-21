// flightlog.cpp — see flightlog.h for the contract.
//
// SPSC queue discipline mirrors cmd.cpp: head is written ONLY by the producer
// (control loop), tail ONLY by the consumer (network task); a full memory
// barrier orders each payload against its index publish. The consumer leaves
// records in the queue between commits — the queue IS the write buffer, and
// the file is opened, appended, and closed per commit so no read handler can
// ever see the active file open twice (littlefs does not support dual handles
// on one file).

#include "flightlog.h"
#include "config.h"
#include "telemetry.h"

#include <LittleFS.h>
#include <esp_partition.h>
#include <string.h>
#include <stdio.h>

// The file body is raw HistRecords; the struct layout IS the on-flash format.
static_assert(sizeof(HistRecord) == 48, "HistRecord changed — bump FLIGHT_LAYOUT_VER and the header");

static const uint32_t FLIGHT_MAGIC      = 0x31474C46;   // "FLG1"
static const uint16_t FLIGHT_LAYOUT_VER = 1;

// Written once at file creation, never rewritten (no in-place updates —
// rewriting a littlefs file head forces a copy of everything after it).
struct FlightHeader {
    uint32_t magic;
    uint16_t layoutVer;
    uint16_t recSize;         // sizeof(HistRecord)
    uint32_t sessionId;
    char     fwVersion[16];   // NUL-padded FIRMWARE_VERSION
    uint8_t  reserved[36];
};
static_assert(sizeof(FlightHeader) == 64, "keep the header a fixed 64 B");

static bool              fsMounted      = false;   // set once in setup, before loop() runs
static volatile bool     logFull        = false;   // written core 0 (service), read core 1 (push)
static char              activeName[12] = "";
static char              activePath[20] = "";      // "/m<N>.bin"
static uint32_t          lastCommitMs   = 0;
static volatile uint32_t droppedCount   = 0;       // written core 1, read core 0 (diag)

// SPSC ring, core 1 → core 0.
static HistRecord        queueRing[FLIGHTLOG_QUEUE_LEN];
static volatile uint16_t qHead = 0;        // producer writes, consumer reads
static volatile uint16_t qTail = 0;        // consumer writes, producer reads

bool        flightlogEnabled()    { return fsMounted && activeName[0] != '\0'; }
bool        flightlogFull()       { return logFull; }
const char* flightlogActiveName() { return activeName; }
uint32_t    flightlogDropped()    { return droppedCount; }

uint32_t flightlogFreeBytes() {
    if (!fsMounted) return 0;
    return (uint32_t)(LittleFS.totalBytes() - LittleFS.usedBytes());
}

// "m<N>.bin" (with or without a leading '/') → N, or 0 if not a flight file.
static uint32_t parseNum(const char* fn) {
    if (!fn) return 0;
    if (*fn == '/') fn++;
    uint32_t n = 0;
    if (sscanf(fn, "m%lu.bin", (unsigned long*)&n) == 1) return n;
    return 0;
}

static void numToPath(uint32_t n, char* path, size_t len) {
    snprintf(path, len, "/m%lu.bin", (unsigned long)n);
}

// Lowest/highest flight-file numbers currently on flash (0 = none).
static void scanNums(uint32_t* lowest, uint32_t* highest) {
    *lowest = 0; *highest = 0;
    File root = LittleFS.open("/");
    if (!root) return;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        uint32_t n = parseNum(f.name());
        if (n == 0) continue;
        if (*lowest == 0 || n < *lowest) *lowest = n;
        if (n > *highest) *highest = n;
    }
}

// Boot-time backstop so the partition can never silently fill and stop
// logging: delete oldest files until the free floor is met. The primary
// clearing path is the app deleting each file after a verified import.
static void pruneToFloor() {
    while (flightlogFreeBytes() < FLIGHTLOG_MIN_FREE_BYTES) {
        uint32_t lo, hi;
        scanNums(&lo, &hi);
        if (lo == 0) break;                     // nothing left to prune
        char path[20];
        numToPath(lo, path, sizeof(path));
        LittleFS.remove(path);
        Serial.printf("[FLIGHTLOG] pruned %s (free floor)\n", path);
    }
}

void flightlogBegin() {
    // Never mount — let alone format — anything but the exact expected
    // partition. A different size means a different flash scheme is on this
    // board; stop and flag, don't guess.
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!part || part->size != FLIGHTLOG_PART_BYTES) {
        Serial.printf("[FLIGHTLOG] WARN: no %lu B \"spiffs\" partition (found %lu) — disabled, nothing touched\n",
                      (unsigned long)FLIGHTLOG_PART_BYTES,
                      part ? (unsigned long)part->size : 0UL);
        return;
    }

    if (!LittleFS.begin(false, "/lfs", 5, "spiffs")) {
        // Region verified above, so an unmountable FS is a first-boot blank
        // region, not a scheme surprise — initialize it exactly once.
        Serial.println("[FLIGHTLOG] no filesystem — formatting verified region (first boot)");
        if (!LittleFS.format() || !LittleFS.begin(false, "/lfs", 5, "spiffs")) {
            Serial.println("[FLIGHTLOG] WARN: format/mount failed — disabled");
            return;
        }
    }
    fsMounted = true;

    pruneToFloor();

    uint32_t lo, hi;
    scanNums(&lo, &hi);
    uint32_t num = hi + 1;
    snprintf(activeName, sizeof(activeName), "m%lu", (unsigned long)num);
    numToPath(num, activePath, sizeof(activePath));

    FlightHeader h = {};
    h.magic     = FLIGHT_MAGIC;
    h.layoutVer = FLIGHT_LAYOUT_VER;
    h.recSize   = (uint16_t)sizeof(HistRecord);
    h.sessionId = telemetrySessionId();
    strlcpy(h.fwVersion, FIRMWARE_VERSION, sizeof(h.fwVersion));

    File f = LittleFS.open(activePath, FILE_WRITE);
    if (!f || f.write((const uint8_t*)&h, sizeof(h)) != sizeof(h)) {
        Serial.println("[FLIGHTLOG] WARN: could not create flight file — disabled");
        if (f) f.close();
        activeName[0] = activePath[0] = '\0';
        return;
    }
    f.close();

    lastCommitMs = millis();
    Serial.printf("[FLIGHTLOG] %s @ 1 Hz, %lu KB free\n",
                  activeName, (unsigned long)(flightlogFreeBytes() / 1024));
}

void flightlogPush(const HistRecord& r) {
    if (!flightlogEnabled() || logFull) return;
    uint16_t h    = qHead;
    uint16_t next = (uint16_t)((h + 1) % FLIGHTLOG_QUEUE_LEN);
    if (next == qTail) { droppedCount++; return; }   // full → drop, never block
    queueRing[h] = r;                                // write payload first…
    __sync_synchronize();                            // …then make it visible before publish
    qHead = next;                                    // publish
}

void flightlogService() {
    if (!flightlogEnabled() || logFull) return;

    uint16_t h = qHead;                              // snapshot the producer index
    if (h == qTail) return;
    uint16_t pending = (uint16_t)((h + FLIGHTLOG_QUEUE_LEN - qTail) % FLIGHTLOG_QUEUE_LEN);
    uint32_t now = millis();
    // Commit on cadence; early if the queue is half full (a stalled task must
    // not cost records — they're the mission log).
    if ((now - lastCommitMs) < FLIGHTLOG_FLUSH_MS && pending < FLIGHTLOG_QUEUE_LEN / 2) return;
    lastCommitMs = now;

    File f = LittleFS.open(activePath, FILE_APPEND);
    if (!f) {
        logFull = true;                              // open failure = stop logging; boat unaffected
        Serial.println("[FLIGHTLOG] WARN: append failed — logging stopped");
        return;
    }
    __sync_synchronize();                            // read payloads written before head publish
    while (qTail != h) {
        if (f.write((const uint8_t*)&queueRing[qTail], sizeof(HistRecord)) != sizeof(HistRecord)) {
            logFull = true;                          // out of space — stored data stays intact
            Serial.println("[FLIGHTLOG] partition full — logging stopped");
            break;
        }
        qTail = (uint16_t)((qTail + 1) % FLIGHTLOG_QUEUE_LEN);
    }
    f.close();                                       // atomic metadata commit
}

uint8_t flightlogList(FlightInfo* out, uint8_t max) {
    if (!fsMounted || max == 0) return 0;
    uint8_t n = 0;
    File root = LittleFS.open("/");
    if (!root) return 0;
    for (File f = root.openNextFile(); f && n < max; f = root.openNextFile()) {
        uint32_t num = parseNum(f.name());
        if (num == 0) continue;
        FlightInfo& fi = out[n];
        snprintf(fi.name, sizeof(fi.name), "m%lu", (unsigned long)num);
        fi.num     = num;
        fi.bytes   = (uint32_t)f.size();
        fi.records = (fi.bytes > sizeof(FlightHeader))
                       ? (uint32_t)((fi.bytes - sizeof(FlightHeader)) / sizeof(HistRecord))
                       : 0;
        fi.active  = (strcmp(fi.name, activeName) == 0);
        // Header comes off the iterator's own handle — littlefs does not
        // support the same file open twice.
        FlightHeader h;
        fi.sessionId = 0;
        if (f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.magic == FLIGHT_MAGIC) {
            fi.sessionId = h.sessionId;
        }
        n++;
    }
    // Oldest-first by number (insertion sort; the list is tiny).
    for (uint8_t i = 1; i < n; i++) {
        FlightInfo key = out[i];
        int8_t j = (int8_t)(i - 1);
        while (j >= 0 && out[j].num > key.num) { out[j + 1] = out[j]; j--; }
        out[j + 1] = key;
    }
    return n;
}

int32_t flightlogRead(const char* name, uint32_t sinceMs, HistRecord* out,
                      uint16_t max, uint32_t* availOut, uint32_t* sessionOut) {
    if (!fsMounted || !name || !name[0]) return -1;
    char path[20];
    snprintf(path, sizeof(path), "/%s.bin", name);
    File f = LittleFS.open(path, FILE_READ);
    if (!f) return -1;

    FlightHeader h;
    if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) ||
        h.magic != FLIGHT_MAGIC || h.recSize != sizeof(HistRecord)) {
        f.close();
        return -1;
    }
    if (sessionOut) *sessionOut = h.sessionId;

    // Whole records only — a torn tail from an unclean power-cut is ignored.
    uint32_t nrec = (uint32_t)((f.size() - sizeof(h)) / sizeof(HistRecord));

    // First record with uptimeMs > sinceMs (records are time-ordered).
    uint32_t lo = 0, hi = nrec;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        HistRecord r;
        f.seek(sizeof(h) + mid * sizeof(HistRecord));
        if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) { f.close(); return -1; }
        if (r.uptimeMs > sinceMs) hi = mid; else lo = mid + 1;
    }
    if (availOut) *availOut = nrec - lo;

    uint16_t cnt = 0;
    f.seek(sizeof(h) + lo * sizeof(HistRecord));
    while (cnt < max && (lo + cnt) < nrec) {
        if (f.read((uint8_t*)&out[cnt], sizeof(HistRecord)) != sizeof(HistRecord)) break;
        cnt++;
    }
    f.close();
    return (int32_t)cnt;
}

bool flightlogDelete(const char* name) {
    if (!fsMounted || !name || !name[0]) return false;
    if (strcmp(name, activeName) == 0) return false;   // never the growing file
    char path[20];
    snprintf(path, sizeof(path), "/%s.bin", name);
    return LittleFS.remove(path);
}
