// sonar.h
// RCWL-1655 depth sonar (NOT JSN-SR04T). Bottom-facing.
//   Range: 2..~450 cm.
//   Modes: off (default), run (auto-pings every DEPTH_RUN_INTERVAL_MS),
//   plus a one-shot check that takes a single ping without changing mode.

#pragma once

#include <Arduino.h>

enum DepthMode {
    DEPTH_OFF = 0,
    DEPTH_RUN = 1,
};

void sonarBegin();
void sonarUpdate();

void sonarSetMode(DepthMode m);
void sonarPingNow();          // one-shot reading

DepthMode sonarMode();
float     sonarLastDepthM();  // -1 if no reading / no echo
uint32_t  sonarLastReadMs();  // 0 if never
