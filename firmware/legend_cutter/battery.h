// battery.h
// INA219 current/voltage sensor — monitors 4S LiPo health.
// Triggers low-voltage alarm and return-to-home when voltage drops below thresholds.

#pragma once
#include <Arduino.h>

struct BatteryData {
  float voltageV;      // Bus voltage (V)
  float currentA;      // Current draw (A)
  float powerW;        // Computed power (W)
  bool  lowVoltage;    // Voltage < BATTERY_ALARM_VOLTAGE
  bool  criticalVoltage; // Voltage < BATTERY_RTH_VOLTAGE → trigger RTH
};

void batteryBegin();
void batteryUpdate();           // Call on a 500 ms interval; non-blocking
const BatteryData& batteryGet();
