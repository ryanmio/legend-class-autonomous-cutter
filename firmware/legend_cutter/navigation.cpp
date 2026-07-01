// navigation.cpp
// Multi-waypoint mission geometry. The active leg is captured when EITHER
// trigger fires:
//   (1) distance: wp_dist < CAPTURE_RADIUS_M
//   (2) crossing: boat passes the perpendicular line at the active waypoint
//       (prevents endless circling when GPS noise is close to the radius).
// On capture the sequencer advances to the next leg and re-records the leg
// start (so the crossing line is perpendicular to the new leg); after the last
// leg missionActive clears and missionComplete latches → neutral hold. Capture
// + leg-start recording run only while AUTO is driving — passing a waypoint in
// MANUAL must not advance the mission.
//
// Approach lock: within AUTO_APPROACH_LOCK_M the steering setpoint latches to
// the bearing held at that moment and stops tracking the instantaneous bearing,
// which goes hypersensitive within a few metres of the point (GPS jitter swings
// it tens of degrees → the boat circles). The boat then drives a straight line
// through the capture zone, which the crossing trigger reliably catches.
//
// A single waypoint is a 1-point mission, so the single-waypoint path is
// unchanged. Cross-core mission handoff mirrors the command ring: the network
// task stages a validated mission, the control loop commits it.

#include "navigation.h"
#include "config.h"
#include "gps.h"
#include <math.h>

// Active mission.
static Waypoint   route[MAX_WAYPOINTS];
static uint8_t    wpCount         = 0;
static uint8_t    wpIdx           = 0;     // active leg (0-based)
static bool       missionActive   = false; // route loaded and a leg still to drive
static bool       missionComplete = false; // last leg captured (sticky until new/clear)

// Active-leg cached geometry.
static float      wpDistM   = 0.0f;
static float      wpBearing = 0.0f;
static CapturedBy capBy     = CAPTURED_BY_NONE;   // how the most recent leg was captured

// Approach-heading lock (latched once inside AUTO_APPROACH_LOCK_M on the leg).
static bool       approachLocked = false;
static float      lockedBearing  = 0.0f;

// Leg-start (recorded on the first AUTO fix after a new leg or AUTO engage).
// Used for the crossing trigger and exposed for app visualisation.
static bool  startValid = false;
static float startLat   = 0.0f;
static float startLon   = 0.0f;

// Mission staging: network task (producer) writes stage[]/stageCount then
// publishes stagePending; the control loop (consumer) commits and clears it.
// Same publish-after-payload discipline as the command ring — one mission
// staged at a time, human-paced.
static Waypoint      stage[MAX_WAYPOINTS];
static uint8_t       stageCount   = 0;
static volatile bool stagePending = false;

// 1 deg of latitude ≈ 111,111 m anywhere on earth. Longitude scales by
// cos(lat). Flat-earth is plenty accurate over a single mission leg.
static const float METERS_PER_DEG_LAT = 111111.0f;
static const float MIN_LEG_M2         = 1.0f;

static float haversineBearing(float fromLat, float fromLon, float toLat, float toLon) {
    float p1 = fromLat * DEG_TO_RAD;
    float p2 = toLat   * DEG_TO_RAD;
    float dl = (toLon - fromLon) * DEG_TO_RAD;
    float y  = sinf(dl) * cosf(p2);
    float x  = cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl);
    float b  = atan2f(y, x) * RAD_TO_DEG;
    return fmodf(b + 360.0f, 360.0f);
}

static float haversineDistM(float fromLat, float fromLon, float toLat, float toLon) {
    const float R = 6371000.0f;
    float p1 = fromLat * DEG_TO_RAD;
    float p2 = toLat   * DEG_TO_RAD;
    float dp = (toLat  - fromLat) * DEG_TO_RAD;
    float dl = (toLon  - fromLon) * DEG_TO_RAD;
    float a  = sinf(dp / 2) * sinf(dp / 2)
             + cosf(p1) * cosf(p2) * sinf(dl / 2) * sinf(dl / 2);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

// True once the boat passes the perpendicular line through waypoint w.
static bool hasCrossedTarget(const Waypoint& w) {
    if (!startValid) return false;
    const float boatLat = gpsLat();
    const float boatLon = gpsLon();
    const float cosLat = cosf(startLat * DEG_TO_RAD);
    const float ax = (w.lon    - startLon) * METERS_PER_DEG_LAT * cosLat;
    const float ay = (w.lat    - startLat) * METERS_PER_DEG_LAT;
    const float legMag2 = ax * ax + ay * ay;
    if (legMag2 < MIN_LEG_M2) return false;
    const float px = (boatLon - startLon) * METERS_PER_DEG_LAT * cosLat;
    const float py = (boatLat - startLat) * METERS_PER_DEG_LAT;
    return (px * ax + py * ay) >= legMag2;
}

// Load an ordered route as the active mission and reset all leg state. Caller
// has already validated count/range. n==0 leaves no active mission.
static void startMission(const Waypoint* pts, uint8_t n) {
    if (n > MAX_WAYPOINTS) n = MAX_WAYPOINTS;
    for (uint8_t i = 0; i < n; i++) route[i] = pts[i];
    wpCount         = n;
    wpIdx           = 0;
    missionActive   = (n > 0);
    missionComplete = false;
    capBy           = CAPTURED_BY_NONE;
    startValid      = false;
    approachLocked  = false;
    wpDistM         = 0.0f;
    wpBearing       = 0.0f;
}

bool navTrySetWaypoint(float lat, float lon, float* outDistM) {
    // Fat-finger guard — only checkable when we know where the boat is.
    // The no-fix case is backstopped in navUpdate().
    if (gpsValid()) {
        float d = haversineDistM(gpsLat(), gpsLon(), lat, lon);
        if (d > MAX_WP_DIST_M) {
            if (outDistM) *outDistM = d;
            Serial.printf("[WP] REJECTED lat=%.6f lon=%.6f — %.0f m away (max %.0f)\n",
                          lat, lon, d, MAX_WP_DIST_M);
            return false;
        }
    }
    Waypoint w = { lat, lon };
    startMission(&w, 1);
    Serial.printf("[WP] set (1-pt mission) lat=%.6f lon=%.6f\n", lat, lon);
    return true;
}

bool navWaypointInRange(float lat, float lon, float* outDistM) {
    if (!gpsValid()) return true;   // no fix → accept; navUpdate() distance-checks later
    float d = haversineDistM(gpsLat(), gpsLon(), lat, lon);
    if (outDistM) *outDistM = d;
    return d <= MAX_WP_DIST_M;
}

bool navMissionInRange(const Waypoint* pts, uint8_t n, uint8_t* badLeg, float* badDist) {
    for (uint8_t i = 0; i < n; i++) {
        float d;
        if (i == 0) {
            if (!gpsValid()) continue;   // no fix → leg-0 backstopped in navUpdate()
            d = haversineDistM(gpsLat(), gpsLon(), pts[0].lat, pts[0].lon);
        } else {
            d = haversineDistM(pts[i - 1].lat, pts[i - 1].lon, pts[i].lat, pts[i].lon);
        }
        if (d > MAX_WP_DIST_M) {
            if (badLeg)  *badLeg  = i;
            if (badDist) *badDist = d;
            return false;
        }
    }
    return true;
}

bool navStageMission(const Waypoint* pts, uint8_t n) {
    if (stagePending)              return false;   // a prior stage hasn't committed yet
    if (n == 0 || n > MAX_WAYPOINTS) return false;
    for (uint8_t i = 0; i < n; i++) stage[i] = pts[i];
    stageCount = n;
    __sync_synchronize();          // payload visible before the publish
    stagePending = true;
    return true;
}

void navCommitStagedMission() {
    if (!stagePending) return;
    __sync_synchronize();          // read payload staged before the publish
    startMission(stage, stageCount);
    stagePending = false;
    Serial.printf("[MISSION] committed %u waypoints, driving leg 1/%u\n", wpCount, wpCount);
}

void navAbortStage() { stagePending = false; }   // producer only; the commit never enqueued

bool navStagePending() { return stagePending; }

void navClearWaypoint() {
    wpCount         = 0;
    wpIdx           = 0;
    missionActive   = false;
    missionComplete = false;
    capBy           = CAPTURED_BY_NONE;
    startValid      = false;
    approachLocked  = false;
    wpDistM         = 0.0f;
    wpBearing       = 0.0f;
    Serial.println("[MISSION] cleared");
}

void navResetLegStart() {
    // Re-record the active leg start at the AUTO-engage position — the crossing
    // line must be perpendicular to the path AUTO will actually drive.
    startValid     = false;
    approachLocked = false;
}

void navUpdate(bool inAuto) {
    if (!missionActive || !gpsValid()) {
        wpDistM   = 0.0f;
        wpBearing = 0.0f;
        return;
    }

    const Waypoint& w = route[wpIdx];
    const float boatLat = gpsLat();
    const float boatLon = gpsLon();
    wpDistM   = haversineDistM(boatLat, boatLon, w.lat, w.lon);
    wpBearing = haversineBearing(boatLat, boatLon, w.lat, w.lon);

    // Capture detection + advance are armed only while AUTO is driving the leg.
    // MANUAL/FAILSAFE still get live dist/bearing telemetry above.
    if (!inAuto) return;

    // Fat-finger backstop, scoped to leg 0 ONLY — the single leg that can reach
    // navUpdate without POST-time range validation (a route posted before the
    // first fix skips the leg-0 check). Later legs were chain-validated at POST,
    // so they must NOT trip this: otherwise GPS jitter near a ~1 km leg, or the
    // documented MANUAL "steer closer to regain WiFi" detour, could erase a whole
    // validated route (RAM-only, unrecoverable without WiFi). AUTO-only now, so a
    // MANUAL excursion never wipes the mission.
    if (wpIdx == 0 && wpDistM > MAX_WP_DIST_M) {
        Serial.printf("[MISSION] aborted — leg 1 %.0f m away (max %.0f)\n",
                      wpDistM, MAX_WP_DIST_M);
        navClearWaypoint();
        return;
    }

    if (!startValid) {
        startLat   = boatLat;
        startLon   = boatLon;
        startValid = true;
        Serial.printf("[MISSION] leg %u/%u start %.6f,%.6f → %.6f,%.6f\n",
                      wpIdx + 1, wpCount, startLat, startLon, w.lat, w.lon);
    }

    // Latch the approach heading once we enter the close zone — past here the
    // instantaneous bearing is too noisy to chase. Hold it through capture.
    if (!approachLocked && wpDistM < AUTO_APPROACH_LOCK_M) {
        approachLocked = true;
        lockedBearing  = wpBearing;
        Serial.printf("[MISSION] approach lock leg %u at %.1f m, heading %.0f deg held.\n",
                      wpIdx + 1, wpDistM, lockedBearing);
    }

    bool captured = false;
    if (wpDistM < CAPTURE_RADIUS_M) {
        captured = true;
        capBy    = CAPTURED_BY_DISTANCE;
    } else if (hasCrossedTarget(w)) {
        captured = true;
        capBy    = CAPTURED_BY_CROSSING;
    }
    if (!captured) return;

    Serial.printf("[MISSION] leg %u/%u captured by %s (dist=%.1f m). ",
                  wpIdx + 1, wpCount,
                  capBy == CAPTURED_BY_DISTANCE ? "DISTANCE" : "CROSSING", wpDistM);
    if (wpIdx + 1 < wpCount) {
        wpIdx++;
        startValid     = false;   // re-record the start of the new leg on next fix
        approachLocked = false;
        capBy          = CAPTURED_BY_NONE;   // that trigger belonged to the finished leg
        // Refresh geometry to the NEW leg now, so applyOutputs() steers toward it
        // this same control cycle instead of the just-passed point for one tick.
        const Waypoint& nw = route[wpIdx];
        wpDistM   = haversineDistM(gpsLat(), gpsLon(), nw.lat, nw.lon);
        wpBearing = haversineBearing(gpsLat(), gpsLon(), nw.lat, nw.lon);
        Serial.printf("Advancing to leg %u/%u.\n", wpIdx + 1, wpCount);
    } else {
        missionActive   = false;
        missionComplete = true;
        Serial.println("MISSION COMPLETE. Outputs neutral.");
    }
}

// A route is loaded — running OR just completed. Stays true after the final
// capture, matching the original single-waypoint contract, so consumers that
// read `captured` inside a `wp_set` guard (helm status, telemetry screen,
// history ring) still see the completion. navMissionActive() is the "still
// driving" flag that goes false at completion.
bool       navWpSet()        { return missionActive || missionComplete; }
float      navWpLat()        { return navWpSet() ? route[wpIdx].lat : 0.0f; }
float      navWpLon()        { return navWpSet() ? route[wpIdx].lon : 0.0f; }
float      navWpDistM()      { return wpDistM; }
float      navWpBearing()    { return wpBearing; }
float      navSteerBearing() { return approachLocked ? lockedBearing : wpBearing; }
bool       navApproachLocked() { return approachLocked; }
bool       navCaptured()     { return missionComplete; }
CapturedBy navCapturedBy()   { return capBy; }
bool       navStartValid()   { return startValid; }
float      navStartLat()     { return startLat; }
float      navStartLon()     { return startLon; }

bool    navMissionActive() { return missionActive; }
uint8_t navWpCount()       { return wpCount; }
uint8_t navWpIdx()         { return wpIdx; }
bool    navWpAt(uint8_t i, float* lat, float* lon) {
    if (i >= wpCount) return false;
    if (lat) *lat = route[i].lat;
    if (lon) *lon = route[i].lon;
    return true;
}
