// weapons.h
// Animated weapon system state machines: deck gun, Phalanx CIWS, and animation modes.
// All weapons controlled from app over WiFi — NOT from RC transmitter.
// State machines run non-blocking in weaponsUpdate(); no delay() calls.

#pragma once
#include <Arduino.h>

enum AnimMode {
  ANIM_NONE,
  ANIM_PATROL_SCAN,    // Weapons sweep alternating arcs; Phalanx spins intermittently
  ANIM_TRACK_TARGET,   // Slew weapons to a bearing at realistic rate (~60°/sec)
  ANIM_COMBAT_DEMO,    // Choreographed sequence timed to DFPlayer sound effects
  ANIM_RANDOM_ALERT,   // Weapons acquire/track imaginary targets with dwell times
  ANIM_LRAD_HAIL,      // Play Coast Guard hailing script through speaker
};

void weaponsBegin();
void weaponsUpdate();                  // Call every loop(); drives all animation state machines

// Manual weapon positioning from app virtual joystick
void weaponsSetGunPan(float deg);     // Degrees from centre, clamped to mechanical limits
void weaponsSetGunTilt(float deg);
void weaponsSetCIWSPan(float deg);
void weaponsSetCIWSSpin(bool on);

// Animation modes
void weaponsSetAnimMode(AnimMode mode);
void weaponsSetTrackBearing(float deg);  // For ANIM_TRACK_TARGET
AnimMode weaponsGetAnimMode();
