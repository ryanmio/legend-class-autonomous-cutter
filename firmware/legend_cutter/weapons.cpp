// weapons.cpp
// Weapon system animation state machines.
// All servo positions expressed in degrees from mechanical centre; clamped to limits.
// Servo microsecond conversion: PWM_NEUTRAL ± (deg / GUN_PAN_RANGE) * GUN_PAN_US_RANGE

#include "weapons.h"
#include "config.h"
#include "motors.h"
#include "audio.h"
#include <Arduino.h>

// ---- Mechanical limits (degrees from centre) ----
#define GUN_PAN_LIMIT    90.0f    // ±90° pan
#define GUN_TILT_UP       15.0f   // +15° up
#define GUN_TILT_DOWN     20.0f   // -20° depression
#define CIWS_PAN_LIMIT   150.0f   // ±150° pan

// ---- Servo degree → microsecond mapping ----
// Assumes 1000 µs = -90°, 1500 µs = 0°, 2000 µs = +90° (standard)
static uint16_t degToUs(float deg, float limitPos, float limitNeg) {
  deg = constrain(deg, -limitNeg, limitPos);
  return (uint16_t)(PWM_NEUTRAL + (deg / 90.0f) * 500.0f);
}

// ---- Current positions ----
static float gunPanDeg  = 0;
static float gunTiltDeg = 0;
static float ciwsPanDeg = 0;

// ---- Animation state ----
static AnimMode animMode        = ANIM_NONE;
static float    trackBearingDeg = 0.0f;
static unsigned long animPhaseMs = 0;
static int8_t   animDir         = 1;  // Sweep direction

void weaponsBegin() {
  // Centre everything on init
  setGunPan(PWM_NEUTRAL);
  setGunTilt(PWM_NEUTRAL);
  setCIWSPan(PWM_NEUTRAL);
  setCIWSSpin(false);
}

void weaponsUpdate() {
  unsigned long now = millis();

  switch (animMode) {
    case ANIM_NONE: break;

    case ANIM_PATROL_SCAN: {
      // Sweep gun pan ±60° at 20°/sec; Phalanx does opposing sweep
      float elapsed = (float)(now - animPhaseMs) / 1000.0f;
      gunPanDeg  = 60.0f * sin(elapsed * 0.33f * PI);
      ciwsPanDeg = -gunPanDeg;
      setCIWSSpin((now / 2000) % 2 == 0);  // Spin on/off every 2 sec
      setGunPan(degToUs(gunPanDeg,  GUN_PAN_LIMIT, GUN_PAN_LIMIT));
      setCIWSPan(degToUs(ciwsPanDeg, CIWS_PAN_LIMIT, CIWS_PAN_LIMIT));
      break;
    }

    case ANIM_TRACK_TARGET: {
      // Slew gun and CIWS toward trackBearingDeg at 60°/sec (realistic)
      // TODO: translate absolute bearing to servo-relative angle using boat heading
      float delta = trackBearingDeg - gunPanDeg;
      float step  = constrain(delta, -1.0f, 1.0f);  // ~60°/sec at 60 Hz loop
      gunPanDeg  += step;
      ciwsPanDeg += step;
      setGunPan(degToUs(gunPanDeg, GUN_PAN_LIMIT, GUN_PAN_LIMIT));
      setCIWSPan(degToUs(ciwsPanDeg, CIWS_PAN_LIMIT, CIWS_PAN_LIMIT));
      break;
    }

    case ANIM_COMBAT_DEMO: {
      // Choreographed multi-phase sequence
      // TODO: implement timed phases with audioPlay() calls
      break;
    }

    case ANIM_RANDOM_ALERT: {
      // Randomly slew to new bearing every 3–8 seconds
      static unsigned long nextTargetMs = 0;
      static float randTarget = 0;
      if (now >= nextTargetMs) {
        randTarget   = (float)(random(-80, 80));
        nextTargetMs = now + random(3000, 8000);
      }
      float delta = randTarget - gunPanDeg;
      gunPanDeg += constrain(delta, -0.5f, 0.5f);
      setGunPan(degToUs(gunPanDeg, GUN_PAN_LIMIT, GUN_PAN_LIMIT));
      break;
    }

    case ANIM_LRAD_HAIL: {
      static bool started = false;
      if (!started) { audioPlay(AUDIO_LRAD_HAIL_1); started = true; }
      // LRAD hail completes when DFPlayer finishes track; reset on mode change
      break;
    }
  }
}

void weaponsSetGunPan(float deg) {
  gunPanDeg = deg;
  setGunPan(degToUs(deg, GUN_PAN_LIMIT, GUN_PAN_LIMIT));
}

void weaponsSetGunTilt(float deg) {
  gunTiltDeg = deg;
  setGunTilt(degToUs(deg, GUN_TILT_UP, GUN_TILT_DOWN));
}

void weaponsSetCIWSPan(float deg) {
  ciwsPanDeg = deg;
  setCIWSPan(degToUs(deg, CIWS_PAN_LIMIT, CIWS_PAN_LIMIT));
}

void weaponsSetCIWSSpin(bool on) { setCIWSSpin(on); }

void weaponsSetAnimMode(AnimMode mode) {
  if (mode != animMode) {
    setCIWSSpin(false);       // Safe stop on mode change
    animMode    = mode;
    animPhaseMs = millis();
  }
}

void weaponsSetTrackBearing(float deg) { trackBearingDeg = deg; }
AnimMode weaponsGetAnimMode()          { return animMode; }
