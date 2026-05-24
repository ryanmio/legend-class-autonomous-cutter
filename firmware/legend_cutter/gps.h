// gps.h
// BN-220 over SoftwareSerial (UART2 is reserved for DF1201S @ 115200).
// Adds bench /sim_gps injection — once set, real GPS is ignored for the
// rest of the session.

#pragma once

#include <Arduino.h>

bool gpsBegin();
void gpsUpdate();             // call every loop()

bool  gpsValid();
float gpsLat();
float gpsLon();
bool  gpsSimulated();
uint8_t gpsSats();
bool  gpsSpeedValid();
float gpsSpeedKnots();
bool  gpsCourseValid();
float gpsCourseDeg();

void  gpsSimSet(float lat, float lon);  // sticky for session
