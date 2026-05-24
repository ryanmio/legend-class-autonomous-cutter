// ibus.h
// Flysky iBUS parser. Single-direction (RX-only) UART1 @ 115200.

#pragma once

#include <Arduino.h>

void     ibusBegin();
void     ibusUpdate();             // call every loop()
uint16_t ibusChannel(uint8_t idx); // 0-based index, returns last good µs (0 if never)
bool     ibusEverGood();           // true once any valid frame has been seen
uint32_t ibusLastFrameMs();        // millis() of last good frame (0 if never)
