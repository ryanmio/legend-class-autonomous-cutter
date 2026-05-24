// bilge.h
// 3 active-LOW water probes (fwd / mid / rear) + 1 active-HIGH pump MOSFET.
// Only the mid sensor (co-located with the pump) runs the pump.

#pragma once

#include <Arduino.h>

void bilgeBegin();
void bilgeUpdate();

bool     bilgeFwdWet();
bool     bilgeMidWet();
bool     bilgeRearWet();
bool     bilgePumpOn();
bool     bilgePumpManual();
bool     bilgeStuck();              // run-dry latch tripped

void     bilgeSetManual(bool on);   // /bilge POST handler
