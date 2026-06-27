// cmd.h
// Lock-free single-producer / single-consumer command queue.
//
// The network task (core 0) is the ONLY producer; it calls cmdEnqueue() from an
// HTTP handler. The control loop (core 1) is the ONLY consumer; it calls
// cmdTryDequeue() once per iteration. This is how operator commands cross from
// the network core to the control core without the control loop ever taking a
// lock or blocking — the loop polls, applies, and moves on. Networking can
// stall on a bad link all it likes; control never waits behind it.

#pragma once

#include <Arduino.h>

enum CmdType : uint8_t {
    CMD_WAYPOINT_SET = 0,
    CMD_WAYPOINT_CLEAR,
    CMD_CRUISE,
    CMD_PID,
    CMD_LED,
    CMD_BILGE,
    CMD_RADAR,
    CMD_DEPTH_MODE,
    CMD_DEPTH_PING,
    CMD_MAGCAL_START,
    CMD_MAGCAL_ABORT,
};

// One flat record. Fields are NOT reused across command types — clarity over
// compactness, because field-reuse is exactly where a dispatch slip would hide.
// Partial-update commands (PID, RADAR) are resolved against current state by
// the handler (cached reads), so the consumer always receives full values.
struct Command {
    CmdType  type;
    float    lat, lon;        // CMD_WAYPOINT_SET
    uint16_t cruiseUs;        // CMD_CRUISE
    float    kp, kd;          // CMD_PID (resolved)
    uint8_t  ledId;           // CMD_LED  (LedId)
    bool     ledOn;           // CMD_LED
    bool     bilgeOn;         // CMD_BILGE
    bool     radarOn;         // CMD_RADAR (resolved)
    uint8_t  radarSpeed;      // CMD_RADAR
    uint32_t radarBurstMs;    // CMD_RADAR
    uint32_t radarPauseMs;    // CMD_RADAR
    bool     depthRun;        // CMD_DEPTH_MODE (true=run, false=stop)
};

// Producer — network task only. Returns false if the ring is full (caller
// should reply 503). Never blocks.
bool cmdEnqueue(const Command& c);

// Consumer — control loop only. Fills out and returns true if a command was
// dequeued; returns false when empty. Never blocks.
bool cmdTryDequeue(Command& out);
