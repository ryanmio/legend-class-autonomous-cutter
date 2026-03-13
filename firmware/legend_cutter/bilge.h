// bilge.h
// Water intrusion detection and bilge pump control.
// Two sensors: forward bilge (GPIO 32) and aft bilge (GPIO 33).
// Pump MOSFET gate driven from GPIO 12 (active HIGH).
// Pump never runs dry: stops after BILGE_DRY_DELAY_MS of dry readings
// and has a hard BILGE_MAX_RUN_MS emergency cutoff.

#pragma once
#include <Arduino.h>

struct BilgeData {
  bool sensorFwd;   // true = water detected forward
  bool sensorAft;   // true = water detected aft
  bool pumpRunning; // true = pump is active
};

void bilgeBegin();
void bilgeUpdate();            // Call every loop(); manages pump state machine
const BilgeData& bilgeGet();
