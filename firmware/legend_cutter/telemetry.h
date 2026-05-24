// telemetry.h
// WiFi (home → hotspot, no AP fallback) + HTTP API + /telemetry JSON.
//
// Endpoints (all CORS-enabled):
//   GET  /status
//   GET  /telemetry
//   POST /cruise     {us | pct}
//   POST /waypoint   {lat,lon}  (lat:null,lon:null clears)
//   POST /pid        {kp?, kd?}
//   POST /sim_gps    {lat,lon}  (bench-only; sticky for session)
//   POST /led        {light:"nav|bridge|deck", state:bool}
//   POST /audio      {sound:"horn|gun|board"}
//   POST /bilge      {on:bool}
//   POST /radar      {on?, speed?, burst_ms?, pause_ms?}
//   POST /depth      {mode:"stop|check|run"}

#pragma once

#include <Arduino.h>

void telemetryBegin();   // brings up WiFi, registers handlers, starts server
void telemetryUpdate();  // server.handleClient()

uint16_t   telemetryCruiseUs();        // last value POSTed to /cruise
const char* telemetryBoatIP();          // c-string; empty if not connected
