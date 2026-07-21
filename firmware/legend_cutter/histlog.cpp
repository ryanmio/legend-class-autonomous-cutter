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
#include "telemetry.h"
#include "flightlog.h"

#include <string.h>
#include <math.h>

// Provided by legend_cutter.ino — same externs telemetry.cpp uses.
extern const char* vesselModeName();
extern bool        vesselFailsafeAck();

static HistRecord ring[HISTLOG_CAPACITY];   // ~58 KB static (.bss)
static uint16_t   head          = 0;        // next write slot
static uint16_t   count         = 0;        // valid records (≤ HISTLOG_CAPACITY)
static uint32_t   seqCounter    = 0;
static uint32_t   lastCaptureMs = 0;

// Uniform-rate retention state. intervalMs starts fine (1 s); after a >20 min
// app absence it flips to coarse (2 s) for the rest of that dropout. `coarsened`
// makes the one-time thin fire exactly once; `coarsening` is the read backstop.
static uint32_t       intervalMs = HISTLOG_INTERVAL_MS;
static bool           coarsened  = false;
static volatile bool  coarsening = false;   // read cross-core by /history (core 0)

void histlogBegin() {
    head = 0;
    count = 0;
    seqCounter = 0;
    lastCaptureMs = 0;
    intervalMs = HISTLOG_INTERVAL_MS;
    coarsened = false;
    coarsening = false;
}

bool histlogCoarsening() { return coarsening; }

// One-time retroactive thin. Keeps every `stride`-th record counting BACK FROM
// THE NEWEST — so the newest survives and the ongoing capture phase lines up
// with the kept data (uniform spacing across the seam). Single forward pass over
// the same anchor: each dest slot (oldest+k) is a logical position already read
// earlier in the pass, so no kept record is overwritten before it is copied —
// no scratch buffer, no shift-under-reader. Runs on core 1 only when the app has
// been gone >20 min, so no /history read is in flight; `coarsening` guards the
// µs window in case a reconnect lands exactly here.
static void coarsenRing(uint16_t stride) {
    if (count == 0) return;
    coarsening = true;
    uint16_t oldest = (head + HISTLOG_CAPACITY - count) % HISTLOG_CAPACITY;
    uint16_t kept   = (uint16_t)((count + stride - 1) / stride);   // ceil(count/stride)
    uint16_t first  = (uint16_t)((count - 1) - (uint16_t)((kept - 1) * stride)); // oldest kept logical idx
    for (uint16_t k = 0; k < kept; k++) {
        uint16_t srcLogical = (uint16_t)(first + k * stride);
        ring[(oldest + k) % HISTLOG_CAPACITY] = ring[(oldest + srcLogical) % HISTLOG_CAPACITY];
    }
    count         = kept;                                          // reduced count published last
    head          = (uint16_t)((oldest + kept) % HISTLOG_CAPACITY);
    lastCaptureMs = ring[(oldest + kept - 1) % HISTLOG_CAPACITY].uptimeMs; // phase-anchor to newest kept
    intervalMs    = HISTLOG_COARSE_MS;
    coarsened     = true;
    coarsening    = false;
}

static uint8_t modeCode(const char* m) {
    if (!strcmp(m, "MANUAL"))   return 1;
    if (!strcmp(m, "AUTO"))     return 2;
    if (!strcmp(m, "FAILSAFE")) return 3;
    return 0;
}

void histlogUpdate() {
    uint32_t now = millis();

    // One-time coarsening: the app has been gone STRICTLY MORE than the window
    // (a dropout of ≤20 min stays 1 s — the boundary is exclusive) and the ring
    // is still fine. Thin what's stored and switch cadence to coarse.
    if (!coarsened && (now - telemetryLastClientMs()) > HISTLOG_COARSEN_AFTER_MS) {
        coarsenRing((uint16_t)(HISTLOG_COARSE_MS / HISTLOG_INTERVAL_MS));
    }
    // Contact resumed after a coarsened dropout → return to 1 s for the NEXT
    // dropout. Stored coarse data is untouched; the app's since_ms cursor bounds
    // future backfills, so recordings never mix rates.
    if (coarsened && (now - telemetryLastClientMs()) < (2u * HISTLOG_INTERVAL_MS)) {
        intervalMs = HISTLOG_INTERVAL_MS;
        coarsened  = false;
    }

    if (lastCaptureMs != 0 && (now - lastCaptureMs) < intervalMs) return;
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
        r.wpIdx    = navWpIdx();
        r.wpCount  = navWpCount();
        // Active-waypoint coords so a backfilled gap can place the leg + markers.
        r.wpLat1e7 = (int32_t)llround((double)navWpLat() * 1e7);
        r.wpLon1e7 = (int32_t)llround((double)navWpLon() * 1e7);
    } else {
        r.wpDist10 = -1;
    }
    if (bilgePumpOn())       flags |= HIST_F_PUMP;
    if (vesselFailsafeAck()) flags |= HIST_F_FAILSAFE_ACK;
    // WiFi link state, published by the core-0 network task (getters below are
    // plain volatile reads — no network call on this core-1 path). rssiDbm stays
    // memset-0 when not associated.
    if (telemetryWifiAssoc()) {
        flags |= HIST_F_WIFI_ASSOC;
        r.rssiDbm = telemetryWifiRssiDbm();
    }
    r.flags = flags;

    ring[head] = r;
    head = (head + 1) % HISTLOG_CAPACITY;
    if (count < HISTLOG_CAPACITY) count++;

    flightlogPush(r);   // second consumer: the full-mission flash log
}

uint16_t histlogCount() { return count; }

const HistRecord* histlogAt(uint16_t i) {
    if (i >= count) return NULL;
    uint16_t oldest = (head + HISTLOG_CAPACITY - count) % HISTLOG_CAPACITY;
    return &ring[(oldest + i) % HISTLOG_CAPACITY];
}
