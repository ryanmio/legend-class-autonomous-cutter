// navigation.cpp
// Autonomous navigation engine.
// Heading error drives rudder + differential thrust via PID controller.
// Waypoint sequencer advances when within WAYPOINT_CAPTURE_M of current target.
// PID gains and home waypoint stored in NVS.

#include "navigation.h"
#include "config.h"
#include "gps.h"
#include "imu.h"
#include "motors.h"
#include <Preferences.h>

static NavMode      navMode       = NAV_IDLE;
static float        targetHeading = 0.0f;
static Waypoint     waypoints[32];
static uint8_t      waypointCount = 0;
static uint8_t      waypointIdx   = 0;
static Waypoint     homeWpt       = {0, 0};
static bool         homeSet       = false;
static Preferences  prefs;

// PID state
static float pidKp = PID_KP_DEFAULT;
static float pidKi = PID_KI_DEFAULT;
static float pidKd = PID_KD_DEFAULT;
static float pidIntegral    = 0.0f;
static float pidPrevError   = 0.0f;
static unsigned long pidLastMs = 0;

void navigationBegin() {
  prefs.begin("nav", true);
  pidKp     = prefs.getFloat("kp", PID_KP_DEFAULT);
  pidKi     = prefs.getFloat("ki", PID_KI_DEFAULT);
  pidKd     = prefs.getFloat("kd", PID_KD_DEFAULT);
  homeWpt.lat = prefs.getDouble("homeLat", 0.0);
  homeWpt.lon = prefs.getDouble("homeLon", 0.0);
  homeSet   = prefs.isKey("homeLat");
  prefs.end();
}

// Wrap angle to -180 to +180 (shortest heading error)
static float wrapError(float err) {
  while (err >  180.0f) err -= 360.0f;
  while (err < -180.0f) err += 360.0f;
  return err;
}

static float computePID(float error) {
  unsigned long now = millis();
  float dt = (now - pidLastMs) / 1000.0f;
  if (dt <= 0.0f || dt > 1.0f) { pidLastMs = now; return 0.0f; }
  pidLastMs = now;

  pidIntegral += error * dt;
  pidIntegral = constrain(pidIntegral, -50.0f, 50.0f);  // Wind-up guard

  float derivative = (error - pidPrevError) / dt;
  pidPrevError = error;

  return pidKp * error + pidKi * pidIntegral + pidKd * derivative;
}

void navigationUpdate() {
  if (navMode == NAV_IDLE) return;

  const GpsData& gps = gpsGet();
  const ImuData& imu = imuGet();

  // Determine target heading for current mode
  float target = targetHeading;

  if (navMode == NAV_WAYPOINT || navMode == NAV_RTH || navMode == NAV_SURVEY) {
    Waypoint* dest = nullptr;
    if (navMode == NAV_RTH && homeSet) {
      dest = &homeWpt;
    } else if (waypointCount > 0 && waypointIdx < waypointCount) {
      dest = &waypoints[waypointIdx];
    }

    if (dest && gps.fix) {
      float dist = gpsDistanceM(gps.lat, gps.lon, dest->lat, dest->lon);
      if (dist < WAYPOINT_CAPTURE_M) {
        if (navMode == NAV_RTH) { navSetMode(NAV_IDLE); return; }
        waypointIdx++;
        if (waypointIdx >= waypointCount) { navSetMode(NAV_IDLE); return; }
        dest = &waypoints[waypointIdx];
      }
      target = gpsBearingDeg(gps.lat, gps.lon, dest->lat, dest->lon);
    }
  }

  float error   = wrapError(target - imu.heading);
  float pidOut  = computePID(error);

  // Map PID output to rudder pulse (centre ± max deflection)
  uint16_t rudderUs = constrain(PWM_NEUTRAL + (int)pidOut * 5, PWM_MIN, PWM_MAX);
  motorsSetAuto(error, PWM_NEUTRAL + 200);  // Fixed cruise throttle for now
}

void navSetMode(NavMode mode) {
  navMode = mode;
  pidIntegral  = 0.0f;
  pidPrevError = 0.0f;
  pidLastMs    = millis();
}

NavMode navGetMode() { return navMode; }

void navSetTargetHeading(float deg) { targetHeading = fmod(deg + 360.0f, 360.0f); }

void navSetWaypoints(const Waypoint* pts, uint8_t count) {
  count = min(count, (uint8_t)32);
  memcpy(waypoints, pts, count * sizeof(Waypoint));
  waypointCount = count;
  waypointIdx   = 0;
}

void navClearWaypoints() { waypointCount = 0; waypointIdx = 0; }

void navSetHome(double lat, double lon) {
  homeWpt.lat = lat;
  homeWpt.lon = lon;
  homeSet     = true;
  prefs.begin("nav", false);
  prefs.putDouble("homeLat", lat);
  prefs.putDouble("homeLon", lon);
  prefs.end();
}

void navTriggerRTH() {
  if (homeSet) navSetMode(NAV_RTH);
}

void navSetPID(float kp, float ki, float kd) {
  pidKp = kp; pidKi = ki; pidKd = kd;
  prefs.begin("nav", false);
  prefs.putFloat("kp", kp);
  prefs.putFloat("ki", ki);
  prefs.putFloat("kd", kd);
  prefs.end();
}
