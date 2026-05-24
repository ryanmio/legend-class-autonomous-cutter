// battery.h
// INA219 voltage + current at 4 Hz. No alarm/RTH thresholds yet (those
// come back in a future test; pool firmware just surfaces telemetry).

#pragma once

#include <Arduino.h>

bool batteryBegin();   // false if INA219 not at INA219_ADDR
void batteryUpdate();

bool  batteryAvailable();
float batteryVolts();
float batteryAmps();
