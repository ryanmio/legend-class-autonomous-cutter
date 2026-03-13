/*
 * legend_cutter.ino
 * Legend Class Autonomous Cutter — ESP32 firmware
 * Firmware version: see FIRMWARE_VERSION in config.h
 *
 * Architecture: non-blocking state machine
 *   States: IDLE → MANUAL → AUTONOMOUS → FAILSAFE | ESTOP | CALIBRATION
 *   iBUS signal from Flysky receiver is parsed every loop().
 *   Mode switch (CH5) transitions between MANUAL and AUTONOMOUS.
 *   iBUS signal loss → FAILSAFE (throttle off, rudder centre).
 *   App e-stop command → ESTOP.
 *
 * OTA: connect to AP, use Arduino IDE → Sketch → Upload via OTA → "legend-cutter"
 *
 * Modules:
 *   ibus      — Flysky iBUS serial parser (UART1)
 *   motors    — PCA9685 ESC + servo control, differential thrust
 *   battery   — INA219 voltage/current
 *   bilge     — water sensors + pump state machine
 *   gps       — BN-220 NMEA parser (UART2)
 *   imu       — ICM-20948 9-DOF sensor fusion
 *   sonar     — JSN-SR04T depth sonar
 *   audio     — DFPlayer Mini MP3 playback
 *   telemetry — WiFi AP + WebSocket + HTTP API
 *   navigation — heading-hold PID, waypoints, RTH
 *   weapons   — deck gun, Phalanx, animation state machines
 */

#include "config.h"
#include "ibus.h"
#include "motors.h"
#include "battery.h"
#include "bilge.h"
#include "gps.h"
#include "imu.h"
#include "sonar.h"
#include "audio.h"
#include "telemetry.h"
#include "navigation.h"
#include "weapons.h"

// ==================== VESSEL STATE MACHINE ====================
enum VesselState {
  STATE_IDLE,
  STATE_MANUAL,
  STATE_AUTONOMOUS,
  STATE_FAILSAFE,
  STATE_ESTOP,
  STATE_CALIBRATION,
};

static VesselState state    = STATE_IDLE;
static bool        eStopReq = false;  // Set by telemetry HTTP /estop handler

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.printf("\n[BOOT] %s firmware v%s\n", VESSEL_NAME, FIRMWARE_VERSION);

  // MOSFET outputs
  pinMode(BILGE_PUMP_PIN,  OUTPUT);
  pinMode(RADAR_MOTOR_PIN, OUTPUT);
  digitalWrite(BILGE_PUMP_PIN,  LOW);
  digitalWrite(RADAR_MOTOR_PIN, LOW);

  // Module init — order matters (Wire must come up before I2C devices)
  motorsBegin();      // PCA9685 init + ESC arm (sends 1500 µs to all channels)
  ibusBegin();        // UART1 for iBUS
  batteryBegin();     // INA219 over I2C
  bilgeBegin();       // Water sensor GPIO + pump pin
  gpsBegin();         // UART2 for GPS
  imuBegin();         // ICM-20948 over I2C (Wire already started by motorsBegin)
  sonarBegin();       // Trigger/echo GPIOs
  audioBegin();       // DFPlayer Mini via SoftwareSerial
  navigationBegin();  // Load PID gains + home waypoint from NVS
  weaponsBegin();     // Centre servos
  telemetryBegin();   // WiFi AP + WebSocket + HTTP server (blocks until AP up)

  state = STATE_IDLE;
  Serial.println("[BOOT] Ready");
}

// ==================== MAIN LOOP ====================
void loop() {
  // --- Always-on: parse iBUS and update sensors regardless of state ---
  ibusUpdate();
  batteryUpdate();
  bilgeUpdate();
  gpsUpdate();
  imuUpdate();
  sonarUpdate();
  audioUpdate();
  telemetryUpdate();
  weaponsUpdate();

  // --- E-stop takes priority over everything ---
  if (eStopReq) {
    state   = STATE_ESTOP;
    eStopReq = false;
  }

  // --- State machine ---
  switch (state) {
    case STATE_IDLE:
      motorsKill();
      // Transition to MANUAL once iBUS signal is present
      if (!ibusSignalLost()) state = STATE_MANUAL;
      break;

    case STATE_MANUAL: {
      if (ibusSignalLost()) { state = STATE_FAILSAFE; break; }
      if (batteryGet().criticalVoltage) { navTriggerRTH(); state = STATE_AUTONOMOUS; break; }

      uint16_t throttle = ibusChannel(IBUS_CH_THROTTLE);
      uint16_t rudder   = ibusChannel(IBUS_CH_RUDDER);
      uint16_t modeSwitch = ibusChannel(IBUS_CH_MODE);

      // Mode switch: >1600 = autonomous
      if (modeSwitch > 1600) { state = STATE_AUTONOMOUS; break; }

      motorsSetManual(throttle, rudder);

      // Throttle → engine audio volume
      float tNorm = (float)(throttle - 1000) / 1000.0f;
      audioSetThrottle(tNorm);
      break;
    }

    case STATE_AUTONOMOUS: {
      if (ibusSignalLost()) { state = STATE_FAILSAFE; break; }

      uint16_t modeSwitch = ibusChannel(IBUS_CH_MODE);
      // RC always wins — flip mode switch back to manual
      if (modeSwitch <= 1600) { state = STATE_MANUAL; break; }

      navigationUpdate();
      break;
    }

    case STATE_FAILSAFE:
      motorsKill();
      audioStop();
      Serial.println("[FAILSAFE] iBUS signal lost");
      // Recover automatically when signal returns
      if (!ibusSignalLost()) state = STATE_MANUAL;
      break;

    case STATE_ESTOP:
      motorsKill();
      audioStop();
      setCIWSSpin(false);
      setRadar(false);
      // Hold until app sends /estop-release (TODO: add HTTP endpoint)
      break;

    case STATE_CALIBRATION:
      // IMU calibration spin — handled by imuCalibrateMag() called from telemetry endpoint
      break;
  }
}

// ==================== E-STOP CALLBACK ====================
// Called from telemetry.cpp HTTP /estop handler
void requestEStop() { eStopReq = true; }
