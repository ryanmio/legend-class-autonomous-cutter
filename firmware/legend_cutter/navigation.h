// navigation.h
// Single-waypoint AUTO: haversine bearing + heading-hold + dual-trigger
// capture (distance OR perpendicular crossing). No multi-waypoint missions
// (pool too small per project memory).

#pragma once

#include <Arduino.h>

enum CapturedBy {
    CAPTURED_BY_NONE = 0,
    CAPTURED_BY_DISTANCE,
    CAPTURED_BY_CROSSING,
};

void navSetWaypoint(float lat, float lon);
void navClearWaypoint();

void navUpdate();   // refresh geometry / capture latch from current GPS

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
