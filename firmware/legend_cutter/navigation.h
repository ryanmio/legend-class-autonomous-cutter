// navigation.h
// Single-waypoint AUTO: haversine bearing + heading-hold + dual-trigger
// capture (distance OR perpendicular crossing). No multi-waypoint missions
// (pool too small per project memory). Capture + leg-start record run only
// while AUTO is actually driving the leg.

#pragma once

#include <Arduino.h>

enum CapturedBy {
    CAPTURED_BY_NONE = 0,
    CAPTURED_BY_DISTANCE,
    CAPTURED_BY_CROSSING,
};

// Try to set a waypoint. Returns false and fills *outDistM if the point is
// farther than MAX_WP_DIST_M from a *valid* current fix (fat-finger guard);
// with no fix it accepts and the geometry update backstops the check later.
bool navTrySetWaypoint(float lat, float lon, float* outDistM);

// Read-only fat-finger check, no state mutation — safe to call from the network
// task. Returns false (and fills *outDistM) if the point is farther than
// MAX_WP_DIST_M from a valid current fix; true with no fix (navUpdate backstops
// it). The actual set is deferred to the control loop via navTrySetWaypoint.
bool navWaypointInRange(float lat, float lon, float* outDistM);

void navClearWaypoint();
void navResetLegStart();   // re-record the leg start on the next AUTO fix

void navUpdate(bool inAuto);   // refresh geometry; capture/leg-start only in AUTO

bool       navWpSet();
float      navWpLat();
float      navWpLon();
float      navWpDistM();
float      navWpBearing();
bool       navCaptured();
CapturedBy navCapturedBy();
bool       navStartValid();
float      navStartLat();
float      navStartLon();
