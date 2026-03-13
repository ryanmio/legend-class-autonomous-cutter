// audio.cpp
// DFPlayer Mini driver using DFRobotDFPlayerMini library.
// Connected via SoftwareSerial (TX=25, RX=26) at 9600 baud.
// Speaker should be mounted close to DFPlayer to minimise ESC noise on wires.

#include "audio.h"
#include "config.h"
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

static SoftwareSerial    dfSerial(DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
static DFRobotDFPlayerMini dfPlayer;
static bool              dfAvailable = false;
static uint8_t           currentVolume = 15;

void audioBegin() {
  dfSerial.begin(9600);
  delay(200);
  if (dfPlayer.begin(dfSerial)) {
    dfAvailable = true;
    dfPlayer.volume(currentVolume);
    Serial.println("[AUDIO] DFPlayer Mini ready");
  } else {
    Serial.println("[AUDIO] DFPlayer Mini not found — check wiring");
  }
}

void audioUpdate() {
  if (!dfAvailable) return;
  // DFRobotDFPlayerMini has no async callback needed; poll available() if desired.
}

void audioPlay(uint8_t track) {
  if (!dfAvailable) return;
  dfPlayer.play(track);
}

void audioStop() {
  if (!dfAvailable) return;
  dfPlayer.stop();
}

void audioSetVolume(uint8_t vol) {
  if (!dfAvailable) return;
  currentVolume = constrain(vol, 0, 30);
  dfPlayer.volume(currentVolume);
}

void audioSetThrottle(float throttle) {
  // Map throttle (0–1) to volume range (8–28) for engine rumble
  uint8_t vol = (uint8_t)(8.0f + throttle * 20.0f);
  audioSetVolume(vol);
}
