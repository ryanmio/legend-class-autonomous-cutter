// navigation.h
// Multi-waypoint AUTO: an ordered mission of up to MAX_WAYPOINTS legs, driven
// one leg at a time. Each leg uses the same proven single-leg geometry —
// haversine bearing + heading-hold + dual-trigger capture (distance OR
// perpendicular crossing) + approach-heading lock. On capture the sequencer
// advances to the next leg (re-recording the leg start so the crossing line is
// perpendicular to the new leg); after the last leg it neutral-holds. A single
// waypoint is just a 1-point mission, so the single-waypoint path is unchanged.
//
// The active-leg accessors (navWpLat/navWpBearing/navCaptured/…) keep their old
// names and report the active leg, so telemetry, the AUTO branch, and the app
// keep working. navCaptured() now means "mission complete" (all legs captured).

#pragma once

#include <Arduino.h>

struct Waypoint { float lat; float lon; };

enum CapturedBy {
    CAPTURED_BY_NONE = 0,
    CAPTURED_BY_DISTANCE,
    CAPTURED_BY_CROSSING,
};

// ── Single waypoint (a 1-point mission) ──────────────────────────────────────
// Try to set a single waypoint. Returns false and fills *outDistM if the point
// is farther than MAX_WP_DIST_M from a *valid* current fix (fat-finger guard);
// with no fix it accepts and the geometry update backstops the check later.
bool navTrySetWaypoint(float lat, float lon, float* outDistM);

// Read-only fat-finger check for a single point, no state mutation — safe to
// call from the network task. Returns false (fills *outDistM) if farther than
// MAX_WP_DIST_M from a valid fix; true with no fix (navUpdate backstops it).
bool navWaypointInRange(float lat, float lon, float* outDistM);

// ── Mission (multi-waypoint) ─────────────────────────────────────────────────
// Cross-core handoff, same SPSC discipline as the command ring: the network
// task validates + stages the mission, then enqueues CMD_MISSION_COMMIT; the
// control loop calls navCommitStagedMission() to copy staging → active route.
bool navStageMission(const Waypoint* pts, uint8_t n);  // producer; false if a stage is still pending
void navCommitStagedMission();                         // consumer (control loop) only
void navAbortStage();                                  // producer; drop a stage that failed to enqueue

// Read-only chain validation for the network handler (no state mutation).
// Every leg must be <= MAX_WP_DIST_M. Leg 0 is measured from the boat if a fix
// exists (skipped with no fix — navUpdate backstops it); leg i>0 from pts[i-1].
// On failure returns false and fills *badLeg (0-based) and *badDist.
bool navMissionInRange(const Waypoint* pts, uint8_t n, uint8_t* badLeg, float* badDist);

void navClearWaypoint();   // clear the whole mission
void navResetLegStart();   // re-record the active leg start on the next AUTO fix

void navUpdate(bool inAuto);   // refresh geometry; capture/advance only in AUTO

// ── Active-leg accessors (report the active leg) ──────────────────────────────
bool       navWpSet();          // a route is loaded (running OR just completed)
float      navWpLat();
float      navWpLon();
float      navWpDistM();
float      navWpBearing();      // true geometric bearing to the active waypoint
float      navSteerBearing();   // bearing AUTO should steer — latched on close approach
bool       navApproachLocked(); // true once inside AUTO_APPROACH_LOCK_M on this leg
bool       navCaptured();       // mission complete (all legs captured)
CapturedBy navCapturedBy();     // how the most recent leg was captured
bool       navStartValid();
float      navStartLat();
float      navStartLon();

// ── Mission state (telemetry / app) ──────────────────────────────────────────
bool    navActiveLegTooFar();   // active leg > MAX_WP_DIST_M → AUTO neutral-holds (no wipe)
bool    navMissionActive();
uint8_t navWpCount();
uint8_t navWpIdx();                                 // 0-based active leg index
bool    navWpAt(uint8_t i, float* lat, float* lon); // read route point i (GET /mission)
