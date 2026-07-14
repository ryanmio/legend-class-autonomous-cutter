// battery.h
// INA219 voltage + current at 4 Hz. No alarm/RTH thresholds yet (those
// come back in a future test; pool firmware just surfaces telemetry).

#pragma once

#include <Arduino.h>

bool batteryBegin();   // false if INA219 not at INA219_ADDR
void batteryUpdate();

bool  batteryAvailable();
float batteryVolts();      // bus + shunt (telemetry convention; shunt reads ~0)
float batteryBusVolts();   // INA219 bus voltage only — the true HV rail (low-volt alarm)
float batteryAmps();
