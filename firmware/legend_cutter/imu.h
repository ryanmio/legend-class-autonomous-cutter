// imu.h
// ICM-20948 9-DOF + complementary-filter heading + PD heading-hold +
// onboard NVS-backed magnetometer calibration + GPS-COG true-heading trim.
//
// Axis remap (confirmed water-test): chip X=up, Y=port, Z=stern →
//   ar_x = ay, ar_y = az, ar_z = ax  (accel)
//   mr_x = -mz, mr_y = -my, mr_z = -mx  (mag)
//
// "True" heading = fused magnetic heading + MAG_DECLINATION_DEG + the learned
// GPS-COG residual trim. navigation steers with it and telemetry reports it,
// because waypoint bearings are true.

#pragma once

#include <Arduino.h>

bool imuBegin();             // false if ICM-20948 not detected; loads mag cal from NVS
void imuUpdate();            // call every loop(): filter + mag-cal tick + D-rate
void imuUpdateCogTrim();     // call every loop(): 1 Hz GPS-COG residual learner

// Heading-hold rudder (µs), clamped to [RUDDER_MIN_US, RUDDER_MAX_US].
// `target` is a TRUE heading; compared against imuHeadingTrue().
uint16_t imuHeadingHoldUs(float target);

// Seed the heading-hold slew from the current rudder on MANUAL/FAILSAFE→AUTO.
void imuResetAutoSteer();

// Raw PD command (µs above neutral): no deadband, slew, or clamp. Drives the
// decoupled AUTO differential-thrust damper that the rudder's deadband can't.
float imuHeadingYawCmd(float target);

// PID gains (live-tunable via /pid).
void  setPidGains(float kp, float kd);
float pidKp();
float pidKd();

// Heading readouts.
bool  imuHeadingReady();     // true once first sample fused
float imuHeadingTrue();      // 0..360°, mag + declination + cog trim (what nav/telemetry use)
float imuHeadingMag();       // 0..360°, fused magnetic only
float imuCogTrim();          // current learned COG residual (deg)

// ── Magnetometer calibration (NVS-backed, app-triggered) ───────────────────
void  imuMagCalBegin();          // start a rotation-coverage cal
bool  imuMagCalAbort();          // true if a cal was actually running (else clears FAILED→IDLE)
bool  imuMagCalCollecting();     // true while collecting

const char* imuMagCalStateName();   // "idle"|"collecting"|"done"|"failed"
int         imuMagCalProgressPct(); // sectors visited / total
bool        imuMagCalibrated();     // baseline>0 && offsets non-zero
uint32_t    imuMagCalTs();          // seconds-since-boot of last successful cal (or NVS value)
bool        imuMagFromNVS();        // true if a real cal was loaded/saved
uint16_t    imuMagCalMask();        // current sector bitmask (only meaningful while collecting)
bool        imuMagCalFailed();      // state == FAILED
const char* imuMagCalFailReason();
const char* imuMagCalQualityName(); // "unknown"|"good"|"fair"|"poor"
bool        imuMagCalQualityKnown();// quality != unknown (gates radius/circ telemetry)
float       imuMagCalRadiusUT();
float       imuMagCalCircPct();
float       imuMagOffX();
float       imuMagOffY();
float       imuMagOffZ();
float       imuMagBaselineUT();
float       imuLiveMagUT();          // live |B| after offset subtraction
