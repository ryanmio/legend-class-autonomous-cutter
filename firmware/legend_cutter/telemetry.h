// telemetry.h
// WiFi (scan-first: home → "iPhone" hotspot → blind fallback, no AP) +
// non-blocking reconnect + HTTP API + /telemetry JSON.
//
// Endpoints (all CORS-enabled):
//   GET  /status
//   GET  /telemetry
//   GET  /history           ?since_ms=<uptimeMs>  (store-and-sync gap backfill;
//                            deep-serves from the flight file past the ring window)
//   GET  /flights           (flash mission-log inventory + health)
//   GET  /flight            ?name=mN&since_ms=<uptimeMs>  (stored flight, /history shape)
//   POST /flight/delete     {name}  (refuses the active file)
//   POST /cruise            {us | pct}
//   POST /waypoint          {lat,lon}  (lat:null,lon:null clears)
//   POST /pid               {kp?, kd?}
//   POST /led               {light:"nav|bridge|deck", state:bool}
//   POST /audio             {sound:"horn|gun|board"}
//   POST /bilge             {on:bool}
//   POST /radar             {on?, speed?, burst_ms?, pause_ms?}
//   POST /depth             {mode:"stop|check|run"}
//   POST /calibrate_mag/start
//   POST /calibrate_mag/abort

#pragma once

#include <Arduino.h>

void telemetryBegin();   // brings up WiFi, registers handlers, starts the core-0 network task

// The boot's session id, generated on first call (flightlogBegin() asks before
// telemetryBegin() runs, so the flight-file header and /telemetry always agree).
uint32_t telemetrySessionId();

uint16_t   telemetryCruiseUs();         // current cruise target (read by the control loop)
void       telemetrySetCruiseUs(uint16_t us);  // applied by the control loop via cmdApply
uint32_t   telemetryNetStackFreeBytes();       // network task worst-case free stack (bench 's' console)
const char* telemetryBoatIP();          // c-string; empty if not connected

// WiFi link state, sampled by the core-0 network task. Plain volatile reads —
// safe to call from the control loop / histlog (core 1) without a network call.
bool       telemetryWifiAssoc();        // true if the STA is associated to the AP
int8_t     telemetryWifiRssiDbm();      // last RSSI dBm when associated, else 0

// millis() of the last app HTTP request (/telemetry or /history), bumped by the
// core-0 handlers. Read cross-core by histlog (core 1) to tell how long the app
// has been gone — the trigger for one-time history coarsening. 0 = none yet.
uint32_t   telemetryLastClientMs();
