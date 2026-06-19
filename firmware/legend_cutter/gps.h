// gps.h
// BN-220 over SoftwareSerial (UART2 is reserved for DF1201S @ 115200).
// Wire colors reversed on this batch: white=TX→GPIO17(RX), green=RX→GPIO4(TX).

#pragma once

#include <Arduino.h>

bool gpsBegin();
void gpsUpdate();             // call every loop()

bool  gpsValid();
float gpsLat();
float gpsLon();
uint8_t gpsSats();
bool  gpsSpeedValid();
float gpsSpeedKnots();
bool  gpsCourseValid();
float gpsCourseDeg();
