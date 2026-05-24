// ibus.cpp
// Flysky iBUS parser. 32-byte frames @ 115200 8N1.
//   bytes 0..1   = 0x20 0x40 (length + cmd header)
//   bytes 2..21  = 10 channels, little-endian uint16
//   bytes 30..31 = checksum = 0xFFFF - sum(bytes 0..29)
//
// RX-only on UART1 (single wire from receiver iBUS SERVO port).

#include "ibus.h"
#include "config.h"

static HardwareSerial ibusSerial(1);
static uint8_t  buf[32];
static uint8_t  idx        = 0;
static uint16_t ch[10]     = {0};
static bool     everGood   = false;
static uint32_t lastFrameMs = 0;

static bool parseFrame() {
    if (buf[0] != 0x20 || buf[1] != 0x40) return false;
    uint16_t sum = 0xFFFF;
    for (int i = 0; i < 30; i++) sum -= buf[i];
    uint16_t rx = buf[30] | (buf[31] << 8);
    if (sum != rx) return false;
    for (int i = 0; i < 10; i++)
        ch[i] = buf[2 + i*2] | (buf[3 + i*2] << 8);
    lastFrameMs = millis();
    everGood = true;
    return true;
}

void ibusBegin() {
    ibusSerial.setRxBufferSize(1024);
    ibusSerial.begin(IBUS_BAUD, SERIAL_8N1, IBUS_RX_PIN, -1);
}

void ibusUpdate() {
    while (ibusSerial.available()) {
        uint8_t b = ibusSerial.read();
        if (idx == 0 && b != 0x20) continue;
        buf[idx++] = b;
        if (idx == 32) { parseFrame(); idx = 0; }
    }
}

uint16_t ibusChannel(uint8_t i)  { return (i < 10) ? ch[i] : 0; }
bool     ibusEverGood()          { return everGood; }
uint32_t ibusLastFrameMs()       { return lastFrameMs; }
