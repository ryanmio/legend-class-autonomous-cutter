// telemetry.h
// WiFi (scan-first: home → "iPhone" hotspot → blind fallback, no AP) +
// non-blocking reconnect + HTTP API + /telemetry JSON.
//
// Endpoints (all CORS-enabled):
//   GET  /status
//   GET  /telemetry
//   GET  /history           ?since_ms=<uptimeMs>  (store-and-sync gap backfill)
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

uint16_t   telemetryCruiseUs();         // current cruise target (read by the control loop)
void       telemetrySetCruiseUs(uint16_t us);  // applied by the control loop via cmdApply
uint32_t   telemetryNetStackFreeBytes();       // network task worst-case free stack (bench 's' console)
const char* telemetryBoatIP();          // c-string; empty if not connected
