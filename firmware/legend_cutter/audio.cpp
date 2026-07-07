// audio.cpp
// DF1201S on HardwareSerial(2) @ 115200 — test_11 proved path.
// The chip silently drops a play command issued while a previous track is
// still playing, so audioPlay() always pauses + 50 ms before issuing the
// new playFileNum (EF commit 54dc3ca).

#include "audio.h"
#include "config.h"
#include <DFRobot_DF1201S.h>

static HardwareSerial   dfSerial(2);
static DFRobot_DF1201S  df;
static bool             ready = false;

bool audioBegin() {
    // Two tries with a full UART re-open between them: if the first handshake
    // comes back empty (a shared-rail sag during the DFPlayer's power-up),
    // reopening the port and asking again catches it once it has settled.
    for (int attempt = 0; attempt < 2; attempt++) {
        dfSerial.begin(DFP_BAUD, SERIAL_8N1, DFP_RX_PIN, DFP_TX_PIN);
        delay(1000);
        bool ok = false;
        uint32_t startMs = millis();
        while (!(ok = df.begin(dfSerial))) {
            if (millis() - startMs > 3000) break;
            delay(250);
        }
        if (ok) {
            df.setVol(DFP_VOLUME);
            df.switchFunction(df.MUSIC);
            delay(2000);
            df.setPlayMode(df.SINGLE);
            df.enableAMP();
            ready = true;
            return true;
        }
        dfSerial.end();
        delay(250);
    }
    return false;
}

bool audioAvailable() { return ready; }

void audioPlay(AudioClip clip) {
    if (!ready) return;
    int16_t track = 0;
    switch (clip) {
      case AUDIO_HORN:  track = DFP_HORN_INDEX;  break;
      case AUDIO_GUN:   track = DFP_GUN_INDEX;   break;
      case AUDIO_BOARD: track = DFP_BOARD_INDEX; break;
    }
    if (track <= 0) return;
    df.pause();
    delay(50);
    df.playFileNum(track);
}
