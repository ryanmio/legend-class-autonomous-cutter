// gps.cpp
// Parses GGA (position/fix) and RMC (speed/course) NMEA sentences from BN-220.
// Uses TinyGPSPlus library for parsing.
// UART2 on GPS_RX_PIN / GPS_TX_PIN at GPS_BAUD.

#include "gps.h"
#include "config.h"
#include <TinyGPSPlus.h>

static HardwareSerial gpsSerial(2);  // UART2
static TinyGPSPlus    tinyGps;
static GpsData        gpsData;

void gpsBegin() {
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  memset(&gpsData, 0, sizeof(gpsData));
}

void gpsUpdate() {
  while (gpsSerial.available()) {
    if (tinyGps.encode(gpsSerial.read())) {
      // New sentence decoded
      if (tinyGps.location.isUpdated() && tinyGps.location.isValid()) {
        gpsData.lat        = tinyGps.location.lat();
        gpsData.lon        = tinyGps.location.lng();
        gpsData.fix        = true;
        gpsData.lastFixMs  = millis();
      }
      if (tinyGps.speed.isUpdated())    gpsData.speedKnots = tinyGps.speed.knots();
      if (tinyGps.course.isUpdated())   gpsData.courseTrue  = tinyGps.course.deg();
      if (tinyGps.satellites.isUpdated()) gpsData.satellites = tinyGps.satellites.value();
    }
  }
  // Mark fix lost if nothing for 5 seconds
  if (gpsData.fix && (millis() - gpsData.lastFixMs > 5000)) {
    gpsData.fix = false;
  }
}

const GpsData& gpsGet() { return gpsData; }

// Haversine great-circle distance in metres
float gpsDistanceM(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  double a = sin(dLat/2)*sin(dLat/2) +
             cos(radians(lat1))*cos(radians(lat2)) *
             sin(dLon/2)*sin(dLon/2);
  return (float)(R * 2.0 * atan2(sqrt(a), sqrt(1.0-a)));
}

// Initial bearing from point 1 → point 2
float gpsBearingDeg(double lat1, double lon1, double lat2, double lon2) {
  double dLon = radians(lon2 - lon1);
  double y = sin(dLon) * cos(radians(lat2));
  double x = cos(radians(lat1))*sin(radians(lat2)) -
             sin(radians(lat1))*cos(radians(lat2))*cos(dLon);
  float bearing = degrees(atan2(y, x));
  return fmod(bearing + 360.0f, 360.0f);
}
