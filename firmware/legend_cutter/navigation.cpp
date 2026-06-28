// navigation.cpp
// Single-waypoint AUTO geometry. `captured` is sticky once EITHER trigger
// fires:
//   (1) distance: wp_dist < CAPTURE_RADIUS_M
//   (2) crossing: boat passes the perpendicular line at the waypoint
//       (prevents endless circling when GPS noise is close to the
//        capture radius).
// Cleared on a new /waypoint. Capture detection + leg-start recording run
// only while AUTO is driving the leg — passing the waypoint in MANUAL must
// not mark the leg complete.
//
// Approach lock: within AUTO_APPROACH_LOCK_M the steering setpoint latches to
// the bearing held at that moment and stops tracking the instantaneous bearing,
// which goes hypersensitive within a few metres of the point (GPS jitter swings
// it tens of degrees → the boat circles). The boat then drives a straight line
// through the capture zone, which the crossing trigger reliably catches.

#include "navigation.h"
#include "config.h"
#include "gps.h"
#include <math.h>

static bool       wpSet      = false;
static float      wpLat      = 0.0f;
static float      wpLon      = 0.0f;
static float      wpDistM    = 0.0f;
static float      wpBearing  = 0.0f;
static bool       captured   = false;
static CapturedBy capBy      = CAPTURED_BY_NONE;

// Approach-heading lock (latched once inside AUTO_APPROACH_LOCK_M on the leg).
static bool       approachLocked = false;
static float      lockedBearing  = 0.0f;

// Leg-start (recorded on the first AUTO fix after /waypoint or AUTO engage).
// Used for the crossing trigger and exposed for app visualisation.
static bool  startValid = false;
static float startLat   = 0.0f;
static float startLon   = 0.0f;

// 1 deg of latitude ≈ 111,111 m anywhere on earth. Longitude scales by
// cos(lat). Flat-earth is plenty accurate over a single pool leg.
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

static bool hasCrossedTarget() {
    if (!startValid) return false;
    const float boatLat = gpsLat();
    const float boatLon = gpsLon();
    const float cosLat = cosf(startLat * DEG_TO_RAD);
    const float ax = (wpLon   - startLon) * METERS_PER_DEG_LAT * cosLat;
    const float ay = (wpLat   - startLat) * METERS_PER_DEG_LAT;
    const float legMag2 = ax * ax + ay * ay;
    if (legMag2 < MIN_LEG_M2) return false;
    const float px = (boatLon - startLon) * METERS_PER_DEG_LAT * cosLat;
    const float py = (boatLat - startLat) * METERS_PER_DEG_LAT;
    return (px * ax + py * ay) >= legMag2;
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
    wpLat      = lat;
    wpLon      = lon;
    wpSet      = true;
    captured   = false;     // a new waypoint always reopens the run
    capBy      = CAPTURED_BY_NONE;
    startValid = false;     // re-record leg start on next GPS update
    approachLocked = false;
    Serial.printf("[WP] set lat=%.6f lon=%.6f\n", wpLat, wpLon);
    return true;
}

bool navWaypointInRange(float lat, float lon, float* outDistM) {
    if (!gpsValid()) return true;   // no fix → accept; navUpdate() distance-checks later
    float d = haversineDistM(gpsLat(), gpsLon(), lat, lon);
    if (outDistM) *outDistM = d;
    return d <= MAX_WP_DIST_M;
}

void navClearWaypoint() {
    wpSet      = false;
    captured   = false;
    capBy      = CAPTURED_BY_NONE;
    startValid = false;
    approachLocked = false;
    wpDistM    = 0.0f;
    wpBearing  = 0.0f;
    Serial.println("[WP] cleared");
}

void navResetLegStart() {
    // Re-record the leg start at the AUTO-engage position — the crossing line
    // must be perpendicular to the path AUTO will actually drive.
    startValid     = false;
    approachLocked = false;
}

void navUpdate(bool inAuto) {
    if (!wpSet || !gpsValid()) {
        wpDistM   = 0.0f;
        wpBearing = 0.0f;
        return;
    }

    const float boatLat = gpsLat();
    const float boatLon = gpsLon();
    wpDistM   = haversineDistM(boatLat, boatLon, wpLat, wpLon);
    wpBearing = haversineBearing(boatLat, boatLon, wpLat, wpLon);

    // Backstop for the fat-finger guard: a waypoint accepted before the first
    // GPS fix gets distance-checked here once position is known.
    if (wpDistM > MAX_WP_DIST_M) {
        Serial.printf("[WP] auto-cleared — %.0f m away (max %.0f)\n", wpDistM, MAX_WP_DIST_M);
        wpSet      = false;
        captured   = false;
        capBy      = CAPTURED_BY_NONE;
        startValid = false;
        wpDistM    = 0.0f;
        wpBearing  = 0.0f;
        return;
    }

    // Capture detection is armed only while AUTO is driving the leg.
    // MANUAL/FAILSAFE still get live dist/bearing telemetry above.
    if (!inAuto) return;

    if (!startValid) {
        startLat   = boatLat;
        startLon   = boatLon;
        startValid = true;
        Serial.printf("[WP] leg start recorded: %.6f, %.6f → %.6f, %.6f\n",
                      startLat, startLon, wpLat, wpLon);
    }

    if (captured) return;

    // Latch the approach heading once we enter the close zone — past here the
    // instantaneous bearing is too noisy to chase. Hold it through capture.
    if (!approachLocked && wpDistM < AUTO_APPROACH_LOCK_M) {
        approachLocked = true;
        lockedBearing  = wpBearing;
        Serial.printf("[WP] approach lock at %.1f m, heading %.0f deg held to capture.\n",
                      wpDistM, lockedBearing);
    }

    if (wpDistM < CAPTURE_RADIUS_M) {
        captured = true;
        capBy    = CAPTURED_BY_DISTANCE;
        Serial.printf("[WP] captured by DISTANCE (dist=%.1f m). Outputs neutral.\n", wpDistM);
        return;
    }
    if (hasCrossedTarget()) {
        captured = true;
        capBy    = CAPTURED_BY_CROSSING;
        Serial.printf("[WP] captured by CROSSING (dist=%.1f m, missed radius). Outputs neutral.\n", wpDistM);
    }
}

bool       navWpSet()        { return wpSet; }
float      navWpLat()        { return wpLat; }
float      navWpLon()        { return wpLon; }
float      navWpDistM()      { return wpDistM; }
float      navWpBearing()    { return wpBearing; }
float      navSteerBearing() { return approachLocked ? lockedBearing : wpBearing; }
bool       navApproachLocked() { return approachLocked; }
bool       navCaptured()     { return captured; }
CapturedBy navCapturedBy()   { return capBy; }
bool       navStartValid()   { return startValid; }
float      navStartLat()     { return startLat; }
float      navStartLon()     { return startLon; }
