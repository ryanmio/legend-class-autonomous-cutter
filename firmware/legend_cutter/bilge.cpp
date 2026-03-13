// bilge.cpp
// Bilge pump state machine:
//   IDLE      → water detected → PUMPING
//   PUMPING   → sensors dry for BILGE_DRY_DELAY_MS → IDLE
//   PUMPING   → BILGE_MAX_RUN_MS elapsed → IDLE (run-dry protection)

#include "bilge.h"
#include "config.h"

static BilgeData bilgeData;
static unsigned long pumpStartMs = 0;
static unsigned long drySinceMs  = 0;
static bool         dryTimerActive = false;

void bilgeBegin() {
  pinMode(WATER_SENSOR_FWD_PIN, INPUT_PULLUP);
  pinMode(WATER_SENSOR_AFT_PIN, INPUT_PULLUP);
  pinMode(BILGE_PUMP_PIN, OUTPUT);
  digitalWrite(BILGE_PUMP_PIN, LOW);
  memset(&bilgeData, 0, sizeof(bilgeData));
}

static void pumpOn() {
  if (!bilgeData.pumpRunning) {
    digitalWrite(BILGE_PUMP_PIN, HIGH);
    bilgeData.pumpRunning = true;
    pumpStartMs = millis();
    dryTimerActive = false;
  }
}

static void pumpOff() {
  digitalWrite(BILGE_PUMP_PIN, LOW);
  bilgeData.pumpRunning = false;
  dryTimerActive = false;
}

void bilgeUpdate() {
  // LOW = water present (sensors pull LOW when wet, resistor keeps HIGH when dry)
  bilgeData.sensorFwd = (digitalRead(WATER_SENSOR_FWD_PIN) == LOW);
  bilgeData.sensorAft = (digitalRead(WATER_SENSOR_AFT_PIN) == LOW);
  bool waterPresent = bilgeData.sensorFwd || bilgeData.sensorAft;
  unsigned long now = millis();

  if (waterPresent) {
    pumpOn();
    dryTimerActive = false;
  }

  if (bilgeData.pumpRunning) {
    // Emergency cutoff — run-dry protection
    if (now - pumpStartMs >= BILGE_MAX_RUN_MS) {
      pumpOff();
      return;
    }
    // Once dry, wait BILGE_DRY_DELAY_MS before stopping
    if (!waterPresent) {
      if (!dryTimerActive) {
        drySinceMs = now;
        dryTimerActive = true;
      } else if (now - drySinceMs >= BILGE_DRY_DELAY_MS) {
        pumpOff();
      }
    }
  }
}

const BilgeData& bilgeGet() { return bilgeData; }
