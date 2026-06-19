// histlog.cpp
// RAM ring buffer of compact telemetry records. See histlog.h for the contract.

#include "histlog.h"
#include "config.h"
#include "motors.h"
#include "imu.h"
#include "gps.h"
#include "navigation.h"
#include "battery.h"
#include "bilge.h"
#include "sonar.h"

#include <string.h>
#include <math.h>

// Provided by legend_cutter.ino — same externs telemetry.cpp uses.
extern const char* vesselModeName();
extern bool        vesselFailsafeAck();

static HistRecord ring[HISTLOG_CAPACITY];   // ~43 KB static (.bss)
static uint16_t   head          = 0;        // next write slot
static uint16_t   count         = 0;        // valid records (≤ HISTLOG_CAPACITY)
static uint32_t   seqCounter    = 0;
static uint32_t   lastCaptureMs = 0;

void histlogBegin() {
    head = 0;
    count = 0;
    seqCounter = 0;
    lastCaptureMs = 0;
}

static uint8_t modeCode(const char* m) {
    if (!strcmp(m, "MANUAL"))   return 1;
    if (!strcmp(m, "AUTO"))     return 2;
    if (!strcmp(m, "FAILSAFE")) return 3;
    return 0;
}

void histlogUpdate() {
    uint32_t now = millis();
    if (lastCaptureMs != 0 && (now - lastCaptureMs) < HISTLOG_INTERVAL_MS) return;
    lastCaptureMs = now;

    HistRecord r;
    memset(&r, 0, sizeof(r));
    r.seq      = ++seqCounter;
    r.uptimeMs = now;
    r.mode     = modeCode(vesselModeName());

    r.escUs     = (int16_t)motorsPortUs();
    r.rudderUs  = (int16_t)motorsRudderUs();
    r.heading10 = (int16_t)lroundf(imuHeadingTrue() * 10.0f);

    uint8_t flags = 0;
    if (gpsValid()) {
        flags |= HIST_F_GPS_FIX;
        // Scale in double — float * 1e7 would shed low digits past 2^24.
        r.lat1e7 = (int32_t)llround((double)gpsLat() * 1e7);
        r.lon1e7 = (int32_t)llround((double)gpsLon() * 1e7);
    }
    r.sats        = (uint8_t)gpsSats();
    r.speedKts100 = gpsSpeedValid()  ? (int16_t)lroundf(gpsSpeedKnots() * 100.0f) : INT16_MIN;
    r.course10    = gpsCourseValid() ? (int16_t)lroundf(gpsCourseDeg()  * 10.0f)  : INT16_MIN;

    if (batteryAvailable()) {
        r.battCv = (int16_t)lroundf(batteryVolts() * 100.0f);
        r.battCa = (int16_t)lroundf(batteryAmps()  * 100.0f);
    } else {
        r.battCv = INT16_MIN;
        r.battCa = INT16_MIN;
    }

    r.depthCm = (sonarLastDepthM() >= 0.0f) ? (int16_t)lroundf(sonarLastDepthM() * 100.0f) : -1;

    if (navWpSet()) {
        flags |= HIST_F_WP_SET;
        if (navCaptured()) flags |= HIST_F_CAPTURED;
        r.wpDist10 = gpsValid() ? (int16_t)lroundf(navWpDistM() * 10.0f) : -1;
    } else {
        r.wpDist10 = -1;
    }
    if (bilgePumpOn())       flags |= HIST_F_PUMP;
    if (vesselFailsafeAck()) flags |= HIST_F_FAILSAFE_ACK;
    r.flags = flags;

    ring[head] = r;
    head = (head + 1) % HISTLOG_CAPACITY;
    if (count < HISTLOG_CAPACITY) count++;
}

uint16_t histlogCount() { return count; }

const HistRecord* histlogAt(uint16_t i) {
    if (i >= count) return NULL;
    uint16_t oldest = (head + HISTLOG_CAPACITY - count) % HISTLOG_CAPACITY;
    return &ring[(oldest + i) % HISTLOG_CAPACITY];
}
