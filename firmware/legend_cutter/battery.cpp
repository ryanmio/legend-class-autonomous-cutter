// battery.cpp
// Adafruit INA219 at INA219_ADDR. Bus voltage + shunt voltage are summed
// to get the true source voltage (the convention test_29 uses).

#include "battery.h"
#include "config.h"
#include <Adafruit_INA219.h>

static Adafruit_INA219 ina(INA219_ADDR);
static bool     ok       = false;
static float    volts    = 0.0f;
static float    busVolts = 0.0f;
static float    amps     = 0.0f;
static uint32_t lastPoll = 0;

bool batteryBegin() {
    ok = ina.begin();
    return ok;
}

void batteryUpdate() {
    if (!ok) return;
    if (millis() - lastPoll < INA_POLL_INTERVAL_MS) return;
    lastPoll = millis();
    busVolts = ina.getBusVoltage_V();
    volts = busVolts + (ina.getShuntVoltage_mV() / 1000.0f);
    amps  = ina.getCurrent_mA() / 1000.0f;
}

bool  batteryAvailable() { return ok; }
float batteryVolts()     { return volts; }
float batteryBusVolts()  { return busVolts; }
float batteryAmps()      { return amps; }
