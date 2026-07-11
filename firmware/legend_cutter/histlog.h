// histlog.h
// Onboard telemetry history — a RAM ring buffer of compact per-second records,
// recorded continuously whether or not WiFi/the app is connected. On reconnect
// the app pulls whatever it missed via GET /history (handler in telemetry.cpp)
// so the flight log stays unbroken across a WiFi dropout.
//
// RAM-only by design: a reboot clears it, but the app already ends a flight on
// a reboot (session_id change), so the ring never holds data the system would
// otherwise have kept. Sizing is in config.h (HISTLOG_CAPACITY/_INTERVAL_MS).
//
// This module only RECORDS and exposes a read API; it never touches outputs,
// WiFi, or the live /telemetry contract — flashing it changes no behavior.

#pragma once

#include <Arduino.h>

// One buffered sample. Numeric optionals use a sentinel (INT16_MIN / -1) when
// the source was unavailable at capture time, so /history can omit them exactly
// as the live /telemetry payload does. lat/lon are valid only when HIST_F_GPS_FIX.
struct HistRecord {
    uint32_t seq;          // monotonic per boot; ordering + the app's page cursor
    uint32_t uptimeMs;     // millis() at capture; app maps to wall-clock on sync
    int32_t  lat1e7;       // degrees × 1e7 (valid only if gps_fix flag set)
    int32_t  lon1e7;
    int32_t  wpLat1e7;     // active waypoint, deg × 1e7 (valid only if WP_SET flag)
    int32_t  wpLon1e7;     // — lets a backfilled gap reconstruct the leg + markers,
                           //   so a multi-waypoint mission plays back unbroken.
    int16_t  heading10;    // true heading, deg × 10 (always present)
    int16_t  course10;     // GPS course, deg × 10  (INT16_MIN = n/a)
    int16_t  speedKts100;  // knots × 100           (INT16_MIN = n/a)
    int16_t  escUs;        // commanded ESC µs
    int16_t  rudderUs;     // commanded rudder µs
    int16_t  battCv;       // bus volts × 100       (INT16_MIN = n/a)
    int16_t  battCa;       // amps × 100            (INT16_MIN = n/a)
    int16_t  depthCm;      // depth, cm             (-1 = no reading)
    int16_t  wpDist10;     // waypoint distance, m × 10 (-1 = n/a)
    uint8_t  mode;         // 0 idle/unknown, 1 MANUAL, 2 AUTO, 3 FAILSAFE
    uint8_t  sats;
    uint8_t  flags;        // HIST_F_* bitfield
    uint8_t  wpIdx;        // active leg, 0-based (valid only if WP_SET flag)
    uint8_t  wpCount;      // total legs in the mission (valid only if WP_SET flag)
    int8_t   rssiDbm;      // STA RSSI dBm (valid only if WIFI_ASSOC flag; 0 = not associated)
};

static const uint8_t HIST_F_GPS_FIX      = 1 << 0;
static const uint8_t HIST_F_WP_SET       = 1 << 1;
static const uint8_t HIST_F_CAPTURED     = 1 << 2;
static const uint8_t HIST_F_PUMP         = 1 << 3;
static const uint8_t HIST_F_FAILSAFE_ACK = 1 << 4;
// Was the boat associated to the AP at capture time? Recorded on every sample so
// a backfilled gap reveals whether the boat left the hotspot (assoc=0) or stayed
// on it while only the app's socket died (assoc=1). Reuses the old struct pad +
// one free flag bit — zero added bytes per record.
static const uint8_t HIST_F_WIFI_ASSOC   = 1 << 5;

void     histlogBegin();                  // reset the ring (call once in setup)
void     histlogUpdate();                 // rate-limited capture (call every loop)
uint16_t histlogCount();                  // valid records currently buffered
const HistRecord* histlogAt(uint16_t i);  // i=0 oldest … count-1 newest; NULL if oob
