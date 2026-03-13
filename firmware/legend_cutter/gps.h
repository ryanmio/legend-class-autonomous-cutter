// gps.h
// BN-220 GPS module (u-blox NEO-M8N) NMEA parser via UART2.
// Extracts position, speed, heading, and satellite count.
// Non-blocking — call gpsUpdate() every loop().

#pragma once
#include <Arduino.h>

struct GpsData {
  double   lat;          // Decimal degrees, positive = N
  double   lon;          // Decimal degrees, positive = E
  float    speedKnots;
  float    courseTrue;   // Degrees from true north (0–360)
  uint8_t  satellites;
  bool     fix;          // true if position fix valid
  unsigned long lastFixMs;
};

void gpsBegin();
void gpsUpdate();              // Feed serial bytes into NMEA parser; call every loop()
const GpsData& gpsGet();

// Haversine distance between two points (metres)
float gpsDistanceM(double lat1, double lon1, double lat2, double lon2);

// Initial bearing from point 1 to point 2 (degrees, 0–360)
float gpsBearingDeg(double lat1, double lon1, double lat2, double lon2);
