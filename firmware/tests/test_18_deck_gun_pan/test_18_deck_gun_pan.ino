/*
 * test_18_deck_gun_pan.ino
 *
 * TWO-PHASE TEST.
 *
 * --- PHASE 1 (current) — channel discovery ---
 * Find which iBUS channel the TX knob is mapped to, by watching for any
 * channel whose value moves frame-to-frame past MOVE_THRESHOLD_US.
 * Operator turns ONLY the dial they want to assign to deck gun pan.
 * Sketch announces which channel(s) are changing.
 *
 * No baseline / no PCA9685 driving in this phase. iBUS only.
 *
 * --- PHASE 2 (TODO, after channel is known) ---
 * Replace the discovery code with:
 *   knob channel (CHn from phase 1) → PCA9685 ch8 (deck gun pan servo).
 * Pass-through with µs clamp, plus a no-frame failsafe that holds
 * pan at neutral when iBUS drops.
 *
 * NOTE: config.h currently says CH_GUN_PAN 3 — that's the pre-hardware
 * scaffold value. Actual wiring is PCA9685 ch8. config.h gets fixed
 * once phase 2 passes.
 *
 * Wiring (phase 1):
 *   Receiver iBUS → 1 kΩ → GPIO 16 (with 2 kΩ to GND)
 *   No I²C / PCA9685 needed yet.
 */

HardwareSerial ibusSerial(1);

uint8_t  ibusBuffer[32];
uint8_t  ibusIdx = 0;
uint16_t channels[10];
uint16_t prevChannels[10];
bool     prevValid = false;

unsigned long lastFrame    = 0;
unsigned long lastWarn     = 0;
unsigned long framesSeen   = 0;
unsigned long lastChanPrint[10] = {0};   // rate limit per channel

const uint16_t      MOVE_THRESHOLD_US     = 20;     // ignore noise smaller than this
const unsigned long PER_CHANNEL_RATE_MS   = 150;    // throttle per-channel "is changing" lines
const unsigned long NO_FRAME_TIMEOUT_MS   = 1000;
const unsigned long FIRST_FRAME_HINT_MS   = 5000;
unsigned long lastHint = 0;

bool parseIBus() {
  if (ibusBuffer[0] != 0x20 || ibusBuffer[1] != 0x40) return false;
  uint16_t sum = 0xFFFF;
  for (int i = 0; i < 30; i++) sum -= ibusBuffer[i];
  uint16_t rx = ibusBuffer[30] | (ibusBuffer[31] << 8);
  if (sum != rx) {
    if (millis() - lastWarn > 1000) {
      Serial.println("[WARN] checksum mismatch — wiring noise?");
      lastWarn = millis();
    }
    return false;
  }
  for (int i = 0; i < 10; i++)
    channels[i] = ibusBuffer[2 + i*2] | (ibusBuffer[3 + i*2] << 8);
  lastFrame = millis();
  framesSeen++;
  return true;
}

void handleFrame() {
  if (!prevValid) {
    // First frame — just stash and announce we're listening. Don't compare
    // (we have no prior frame to diff against).
    for (int i = 0; i < 10; i++) prevChannels[i] = channels[i];
    prevValid = true;
    Serial.println();
    Serial.println("[OK] iBUS frames arriving. Watching for channel changes.");
    Serial.print("     Initial values: ");
    for (int i = 0; i < 10; i++) {
      Serial.printf("CH%d=%u ", i + 1, channels[i]);
    }
    Serial.println();
    Serial.println("Now turn ONLY the knob you want to assign. Lines below");
    Serial.println("identify the channel that's moving.");
    Serial.println("----------------------------------------");
    return;
  }

  unsigned long now = millis();
  for (int i = 0; i < 10; i++) {
    int delta = (int)channels[i] - (int)prevChannels[i];
    int absDelta = delta < 0 ? -delta : delta;
    if (absDelta > MOVE_THRESHOLD_US) {
      if (now - lastChanPrint[i] >= PER_CHANNEL_RATE_MS) {
        Serial.printf("CH%d (idx %d) is changing:  %u → %u  (Δ %+d)\n",
                      i + 1, i, prevChannels[i], channels[i], delta);
        lastChanPrint[i] = now;
      }
      // Update prev only when we actually report — keeps the delta
      // meaningful relative to the last-reported value rather than
      // shrinking to ~0 on every consecutive frame.
      prevChannels[i] = channels[i];
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_18_deck_gun_pan — phase 1: channel discovery");
  Serial.println("========================================");
  Serial.println("Goal: find which iBUS channel your TX knob is on.");
  Serial.println();
  Serial.println("Procedure:");
  Serial.println("  1. Turn on transmitter. Hands off the sticks.");
  Serial.println("  2. Wait for [OK] iBUS frames arriving.");
  Serial.println("  3. Slowly turn ONLY the dial / knob you want to use.");
  Serial.println("  4. Read the channel number from the 'CHn is changing' line.");
  Serial.println();
  Serial.printf("Move threshold: %u µs (smaller deltas treated as noise).\n",
                MOVE_THRESHOLD_US);
  Serial.println();

  ibusSerial.begin(115200, SERIAL_8N1, 16, -1);

  Serial.println("Waiting for iBUS frames...");
  Serial.println("----------------------------------------");
  lastHint = millis();
}

void loop() {
  while (ibusSerial.available()) {
    uint8_t b = ibusSerial.read();
    if (ibusIdx == 0 && b != 0x20) continue;
    ibusBuffer[ibusIdx++] = b;
    if (ibusIdx == 32) {
      if (parseIBus()) handleFrame();
      ibusIdx = 0;
    }
  }

  // No-frame hint while waiting for first frame.
  if (!prevValid && millis() - lastHint > FIRST_FRAME_HINT_MS) {
    Serial.println("  ...still waiting. Is the transmitter on? Receiver bound?");
    lastHint = millis();
  }

  // Frame-loss notice once we've seen at least one frame.
  if (prevValid && lastFrame > 0 &&
      millis() - lastFrame > NO_FRAME_TIMEOUT_MS) {
    if (millis() - lastWarn > 2000) {
      Serial.println("[WARN] no iBUS frames for >1 s. Check TX / RX power and wiring.");
      lastWarn = millis();
    }
  }
}
