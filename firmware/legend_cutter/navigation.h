// navigation.h
// Autonomous navigation: heading-hold PID, waypoint following, return-to-home.
// Phase 3 feature — stubs in place, implement after Phase 2 sensors are verified.

#pragma once
#include <Arduino.h>

// A single GPS waypoint
struct Waypoint {
  double lat;
  double lon;
};

// Navigation states
enum NavMode {
  NAV_IDLE,
  NAV_HEADING_HOLD,   // Maintain a fixed compass heading
  NAV_WAYPOINT,       // Follow a sequence of waypoints
  NAV_RTH,            // Return to home waypoint
  NAV_SURVEY,         // Execute a lawnmower survey grid
};

void navigationBegin();
void navigationUpdate();               // Call every loop() in AUTONOMOUS state

void navSetMode(NavMode mode);
NavMode navGetMode();

// Heading hold
void navSetTargetHeading(float deg);

// Waypoint mission
void navSetWaypoints(const Waypoint* pts, uint8_t count);
void navClearWaypoints();

// Set/get home waypoint (saved to NVS on set)
void navSetHome(double lat, double lon);
void navTriggerRTH();

// PID tuning (call from app Settings; saves to NVS)
void navSetPID(float kp, float ki, float kd);
