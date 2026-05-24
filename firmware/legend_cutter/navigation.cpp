// navigation.cpp
// Single-waypoint AUTO geometry. `captured` is sticky once EITHER trigger
// fires:
//   (1) distance: wp_dist < CAPTURE_RADIUS_M
//   (2) crossing: boat passes the perpendicular line at the waypoint
//       (prevents endless circling when GPS noise is close to the
//        capture radius).
// Cleared on a new /waypoint.

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

// Leg-start (first GPS fix after /waypoint). Used for crossing trigger
// and exposed for app visualisation.
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

void navSetWaypoint(float lat, float lon) {
    wpLat      = lat;
    wpLon      = lon;
    wpSet      = true;
    captured   = false;
    capBy      = CAPTURED_BY_NONE;
    startValid = false;   // re-record leg start on next GPS update
}

void navClearWaypoint() {
    wpSet      = false;
    captured   = false;
    capBy      = CAPTURED_BY_NONE;
    startValid = false;
    wpDistM    = 0.0f;
    wpBearing  = 0.0f;
}

void navUpdate() {
    if (!wpSet || !gpsValid()) {
        wpDistM   = 0.0f;
        wpBearing = 0.0f;
        return;
    }

    const float boatLat = gpsLat();
    const float boatLon = gpsLon();
    wpDistM   = haversineDistM(boatLat, boatLon, wpLat, wpLon);
    wpBearing = haversineBearing(boatLat, boatLon, wpLat, wpLon);

    if (!startValid) {
        startLat   = boatLat;
        startLon   = boatLon;
        startValid = true;
    }

    if (captured) return;
    if (wpDistM < CAPTURE_RADIUS_M) {
        captured = true;
        capBy    = CAPTURED_BY_DISTANCE;
        return;
    }
    if (hasCrossedTarget()) {
        captured = true;
        capBy    = CAPTURED_BY_CROSSING;
    }
}

bool       navWpSet()        { return wpSet; }
float      navWpLat()        { return wpLat; }
float      navWpLon()        { return wpLon; }
float      navWpDistM()      { return wpDistM; }
float      navWpBearing()    { return wpBearing; }
bool       navCaptured()     { return captured; }
CapturedBy navCapturedBy()   { return capBy; }
bool       navStartValid()   { return startValid; }
float      navStartLat()     { return startLat; }
float      navStartLon()     { return startLon; }
