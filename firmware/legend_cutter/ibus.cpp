// ibus.cpp
// iBUS serial protocol parser for Flysky FS-iA10B receiver.
//
// Protocol: 32-byte frame at 115200 baud, 8N1
//   Byte 0:    0x20 (frame length)
//   Byte 1:    0x40 (command)
//   Bytes 2-21: 10 channels × 2 bytes little-endian (1000–2000)
//   Bytes 22-23: checksum (0xFFFF - sum of bytes 0-21)
//
// The receiver's iBUS SERVO port outputs this frame continuously at ~50 Hz.
// Voltage level is 5V — use a 1K+2K voltage divider before ESP32 GPIO.

#include "ibus.h"
#include "config.h"

static HardwareSerial ibusSerial(1);  // UART1
static IBusData       ibusData;
static uint8_t        ibusBuffer[32];
static uint8_t        ibusIdx = 0;

void ibusBegin() {
  ibusSerial.begin(IBUS_BAUD, SERIAL_8N1, IBUS_RX_PIN, -1);  // RX only
  memset(&ibusData, 0, sizeof(ibusData));
  // Safe defaults: all channels at neutral
  for (int i = 0; i < 10; i++) ibusData.channels[i] = 1500;
}

static bool ibusParseFrame() {
  if (ibusBuffer[0] != 0x20 || ibusBuffer[1] != 0x40) return false;

  // Verify checksum
  uint16_t sum = 0xFFFF;
  for (int i = 0; i < 30; i++) sum -= ibusBuffer[i];
  uint16_t rxChecksum = ibusBuffer[30] | (ibusBuffer[31] << 8);
  if (sum != rxChecksum) return false;

  // Extract channels
  for (int ch = 0; ch < 10; ch++) {
    int offset = 2 + ch * 2;
    ibusData.channels[ch] = ibusBuffer[offset] | (ibusBuffer[offset + 1] << 8);
  }
  ibusData.valid = true;
  ibusData.lastFrameMs = millis();
  return true;
}

void ibusUpdate() {
  while (ibusSerial.available()) {
    uint8_t b = ibusSerial.read();

    // Sync on frame header 0x20
    if (ibusIdx == 0 && b != 0x20) continue;
    ibusBuffer[ibusIdx++] = b;

    if (ibusIdx == 32) {
      ibusParseFrame();
      ibusIdx = 0;
    }
  }
}

const IBusData& ibusGet() { return ibusData; }

bool ibusSignalLost() {
  return (millis() - ibusData.lastFrameMs) > IBUS_FAILSAFE_MS;
}

uint16_t ibusChannel(uint8_t ch) {
  if (ch >= 10) return 1500;
  return ibusData.channels[ch];
}
