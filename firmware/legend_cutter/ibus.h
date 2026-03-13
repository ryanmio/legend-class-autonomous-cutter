// ibus.h
// iBUS receiver parser — reads all 10 channels from Flysky FS-iA10B via UART1.
// Non-blocking: call ibusUpdate() every loop iteration.
// The iBUS frame is 32 bytes at 115200 baud over a single wire (RX only).

#pragma once
#include <Arduino.h>

// Raw channel values in the iBUS range 1000–2000 µs
struct IBusData {
  uint16_t channels[10];
  bool     valid;             // true if last frame passed checksum
  unsigned long lastFrameMs;  // millis() of most recent valid frame
};

void ibusBegin();
void ibusUpdate();                      // Call every loop(); non-blocking
const IBusData& ibusGet();             // Returns current channel snapshot
bool ibusSignalLost();                 // True if no valid frame for IBUS_FAILSAFE_MS
uint16_t ibusChannel(uint8_t ch);     // Convenience: get single channel value (0-based)
