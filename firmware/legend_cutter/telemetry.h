// telemetry.h
// WiFi Access Point + WebSocket telemetry server + HTTP command API.
// ESP32 hosts a WiFi AP; the iOS app connects directly (no router needed).
// Telemetry JSON broadcast over WebSocket (port 81) at TELEMETRY_INTERVAL_MS.
// HTTP API (port 80) handles command endpoints from the app.

#pragma once
#include <Arduino.h>

void telemetryBegin();
void telemetryUpdate();   // Call every loop(); handles WebSocket and HTTP events

// Enqueue a JSON telemetry broadcast — called from main loop on timer
void telemetryBroadcast();

// Register HTTP command handlers (called once from telemetryBegin)
void telemetryRegisterRoutes();
