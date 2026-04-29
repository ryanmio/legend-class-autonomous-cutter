/*
 * test_12_led_lights.ino
 *
 * Bench/boat test. Cycles three LED lighting circuits driven directly
 * from ESP32 GPIO pins (no MOSFET, no PCA9685). Each circuit lights
 * for 3 s in sequence, then all three light together for 3 s, then
 * the sketch prints PASS and stops.
 *
 * Pinout:
 *   GPIO 18 → Nav lights         (port red + starboard green sidelights)
 *   GPIO 19 → Bridge/interior    (cabin/wheelhouse glow)
 *   GPIO 23 → Deck/flood lights  (working deck illumination)
 *
 * These pins were chosen because they are general-purpose outputs,
 * not strapping pins, not input-only, and not in use by iBUS, GPS,
 * I2C, PCA9685, DFPlayer, or ESC/servo on this build.
 *
 * Wiring (each circuit, repeat per pin):
 *   ESP32 GPIO ── [current-limit R] ── LED(+) ──...── LED(-) ── ESP32 GND
 *
 * Direct-drive current budget:
 *   - ESP32 GPIO is rated 12 mA continuous, 40 mA absolute max.
 *   - Size R so each circuit draws ≤ 12 mA at 3.3 V to stay in the
 *     safe band. For a single 3 V red LED that's R ≈ 220 Ω; for a
 *     blue/white LED at ~3.0-3.2 Vf, R ≈ 47-100 Ω.
 *   - For multi-LED strings or 5 V strips, add a logic-level MOSFET
 *     between GPIO and the LED return — direct drive will sag the
 *     pin and dim/flicker the LEDs.
 *
 * Test flow (one-shot, no looping):
 *   1. Nav lights on for 3 s
 *   2. Bridge/interior on for 3 s
 *   3. Deck/flood on for 3 s
 *   4. All three on together for 3 s
 *   5. Print PASS, idle in loop()
 *
 * Reset the board to run the sequence again.
 */

const uint8_t PIN_NAV    = 18;
const uint8_t PIN_BRIDGE = 19;
const uint8_t PIN_DECK   = 23;
const unsigned long ON_MS = 3000;

void runCircuit(const char *name, uint8_t pin) {
  Serial.printf("[ON ]  %-16s  GPIO%2d\n", name, pin);
  digitalWrite(pin, HIGH);
  delay(ON_MS);
  digitalWrite(pin, LOW);
  Serial.printf("[OFF]  %-16s  GPIO%2d\n", name, pin);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========================================");
  Serial.println("  test_12_led_lights");
  Serial.println("========================================");
  Serial.printf("Nav lights      GPIO%d\n", PIN_NAV);
  Serial.printf("Bridge/interior GPIO%d\n", PIN_BRIDGE);
  Serial.printf("Deck/flood      GPIO%d\n", PIN_DECK);
  Serial.println();

  pinMode(PIN_NAV,    OUTPUT);
  pinMode(PIN_BRIDGE, OUTPUT);
  pinMode(PIN_DECK,   OUTPUT);
  digitalWrite(PIN_NAV,    LOW);
  digitalWrite(PIN_BRIDGE, LOW);
  digitalWrite(PIN_DECK,   LOW);

  Serial.println("Step 1: cycling each circuit individually for 3 s.");
  Serial.println("----------------------------------------");
  runCircuit("Nav lights",      PIN_NAV);
  runCircuit("Bridge/interior", PIN_BRIDGE);
  runCircuit("Deck/flood",      PIN_DECK);

  Serial.println();
  Serial.println("Step 2: all three on together for 3 s.");
  Serial.println("----------------------------------------");
  digitalWrite(PIN_NAV,    HIGH);
  digitalWrite(PIN_BRIDGE, HIGH);
  digitalWrite(PIN_DECK,   HIGH);
  Serial.println("[ON ]  ALL THREE       GPIO18,19,23");
  delay(ON_MS);
  digitalWrite(PIN_NAV,    LOW);
  digitalWrite(PIN_BRIDGE, LOW);
  digitalWrite(PIN_DECK,   LOW);
  Serial.println("[OFF]  ALL THREE       GPIO18,19,23");

  Serial.println();
  Serial.println("========================================");
  Serial.println("  PASS — all three circuits cycled OK");
  Serial.println("========================================");
  Serial.println("Reset the board to run the sequence again.");
}

void loop() {
  // One-shot test. Everything ran in setup(). Idle here forever.
}
