// battery.cpp
// INA219 battery monitor via I2C.
// Uses Adafruit INA219 library. I2C address set to INA219_ADDR (0x41) via A0 jumper.

#include "battery.h"
#include "config.h"
#include <Adafruit_INA219.h>

static Adafruit_INA219 ina219(INA219_ADDR);
static BatteryData battData;
static unsigned long lastReadMs = 0;

void batteryBegin() {
  if (!ina219.begin()) {
    Serial.println("[BATTERY] INA219 not found — check wiring and address");
  }
  // Configure for 32V / 2A range (4S LiPo max ~16.8 V, typical draw < 30 A peak)
  // For high-current applications consider calibrating manually.
  ina219.setCalibration_32V_2A();
  memset(&battData, 0, sizeof(battData));
  battData.voltageV = 14.8f; // Safe default until first read
}

void batteryUpdate() {
  unsigned long now = millis();
  if (now - lastReadMs < 500) return;
  lastReadMs = now;

  battData.voltageV  = ina219.getBusVoltage_V() + (ina219.getShuntVoltage_mV() / 1000.0f);
  battData.currentA  = ina219.getCurrent_mA() / 1000.0f;
  battData.powerW    = battData.voltageV * battData.currentA;
  battData.lowVoltage      = battData.voltageV < BATTERY_ALARM_VOLTAGE;
  battData.criticalVoltage = battData.voltageV < BATTERY_RTH_VOLTAGE;
}

const BatteryData& batteryGet() { return battData; }
