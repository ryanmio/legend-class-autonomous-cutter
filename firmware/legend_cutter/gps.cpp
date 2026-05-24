// gps.cpp
// BN-220 on SoftwareSerial @ 9600. Same physical pins as before; we
// chose SoftwareSerial because UART2 is reserved for the DF1201S audio
// module at 115200 (SoftwareSerial @ 115200 + WiFi is unreliable, test_11).

#include "gps.h"
#include "config.h"
#include <SoftwareSerial.h>
#include <TinyGPSPlus.h>

static SoftwareSerial gpsSerial(GPS_RX_PIN, GPS_TX_PIN);
static TinyGPSPlus    tgps;

static float boatLat = 0.0f;
static float boatLon = 0.0f;
static bool  valid       = false;
static bool  simulated   = false;

bool gpsBegin() {
    gpsSerial.begin(GPS_BAUD);
    return true;
}

void gpsUpdate() {
    while (gpsSerial.available()) tgps.encode(gpsSerial.read());
    if (simulated) return;
    if (tgps.location.isValid() && tgps.location.age() < 5000) {
        boatLat = (float)tgps.location.lat();
        boatLon = (float)tgps.location.lng();
        valid   = true;
    } else if (valid) {
        // Fix went stale (>5 s); clear so AUTO safe-holds at neutral
        // rather than steering on frozen coordinates.
        valid = false;
    }
}

bool  gpsValid()        { return valid; }
float gpsLat()          { return boatLat; }
float gpsLon()          { return boatLon; }
bool  gpsSimulated()    { return simulated; }
uint8_t gpsSats()       { return (uint8_t)tgps.satellites.value(); }
bool  gpsSpeedValid()   { return tgps.speed.isValid(); }
float gpsSpeedKnots()   { return tgps.speed.knots(); }
bool  gpsCourseValid()  { return tgps.course.isValid(); }
float gpsCourseDeg()    { return tgps.course.deg(); }

void gpsSimSet(float lat, float lon) {
    boatLat   = lat;
    boatLon   = lon;
    valid     = true;
    simulated = true;
}
