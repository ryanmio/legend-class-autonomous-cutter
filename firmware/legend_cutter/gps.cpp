// gps.cpp
// BN-220 on SoftwareSerial @ 9600. SoftwareSerial (not UART2) because UART2
// is reserved for the DF1201S audio module at 115200 (SoftwareSerial @
// 115200 + WiFi is unreliable, test_11). Mirrors test_29 updatePosition().

#include "gps.h"
#include "config.h"
#include <SoftwareSerial.h>
#include <TinyGPSPlus.h>

static SoftwareSerial gpsSerial(GPS_RX_PIN, GPS_TX_PIN);
static TinyGPSPlus    tgps;

static float boatLat = 0.0f;
static float boatLon = 0.0f;
static bool  valid   = false;

bool gpsBegin() {
    gpsSerial.begin(GPS_BAUD);
    return true;
}

void gpsUpdate() {
    while (gpsSerial.available()) tgps.encode(gpsSerial.read());
    if (tgps.location.isValid() && tgps.location.age() < 5000) {
        boatLat = (float)tgps.location.lat();
        boatLon = (float)tgps.location.lng();
        valid   = true;
    } else if (valid) {
        // Fix went stale (>5 s); clear so AUTO safe-holds at neutral rather
        // than steering on frozen coordinates.
        valid = false;
        Serial.println("[GPS] fix lost (age > 5s) — AUTO will safe-hold until fix returns");
    }
}

bool  gpsValid()        { return valid; }
float gpsLat()          { return boatLat; }
float gpsLon()          { return boatLon; }
uint8_t gpsSats()       { return (uint8_t)tgps.satellites.value(); }
bool  gpsSpeedValid()   { return tgps.speed.isValid(); }
float gpsSpeedKnots()   { return tgps.speed.knots(); }
// TinyGPS++ ignores empty NMEA terms, so an RMC with no track angle leaves
// course holding whatever it last parsed — observed latching the ddmmyy date
// for a whole boot while isValid() stayed true. Range-check at the source so
// every consumer (COG trim, histlog, /telemetry) sees it as absent instead.
bool gpsCourseValid() {
    if (!tgps.course.isValid()) return false;
    float c = tgps.course.deg();
    return c >= 0.0f && c < 360.0f;
}
float gpsCourseDeg()    { return tgps.course.deg(); }
