// flightlog.h
// Full-mission flash log — appends the same 48 B HistRecord the RAM ring
// captures to a growing file on LittleFS (the "spiffs" partition), one file
// per boot, always on. This is the layer beneath the ring: the ring serves
// live gap backfill, this file makes the whole mission survive a crash or
// field power-off and never overflow on a long run.
//
// Crash contract: LittleFS commits are atomic (copy-on-write), so an unclean
// power-cut loses at most the records queued since the last commit (one
// FLIGHTLOG_FLUSH_MS window) and can never corrupt earlier files or brick
// boot. The reader truncates a torn tail to a whole-record boundary — that is
// the entire integrity mechanism.
//
// Partition contract: the module verifies the "spiffs" partition exists at
// exactly FLIGHTLOG_PART_BYTES before touching it. On mismatch it disables
// itself and NEVER formats — a format only ever initializes the verified
// region on its first boot.
//
// Ownership/concurrency: flightlogBegin() runs in setup(), before the network
// task exists. All later filesystem access happens on core 0 — the writer
// (flightlogService, called from networkTask's loop) and the HTTP read/delete
// handlers run in that same task, so the FS is single-threaded by
// construction and the active file is never open twice at once (the writer
// opens, appends, and closes per commit). The control loop only ever touches
// the lock-free SPSC queue.

#pragma once

#include <Arduino.h>
#include "histlog.h"

void flightlogBegin();                    // mount+prune+create; setup() only, pre-network-task
void flightlogPush(const HistRecord& r);  // producer, core 1 (histlogUpdate); never blocks
void flightlogService();                  // consumer, core 0 (networkTask); commits on cadence

bool        flightlogEnabled();           // FS mounted and this boot's file exists
bool        flightlogFull();              // partition filled / write failed — logging stopped, boat unaffected
const char* flightlogActiveName();        // e.g. "m17"; "" if disabled
uint32_t    flightlogFreeBytes();         // LittleFS free space (0 if not mounted)
uint32_t    flightlogDropped();           // records dropped on a full queue (expect 0)

// One stored flight file, for GET /flights.
struct FlightInfo {
    char     name[12];        // "m<N>"
    uint32_t num;             // <N>; files are listed ascending by this
    uint32_t sessionId;       // session_id of the boot that recorded it
    uint32_t records;         // whole records on flash (torn tail excluded)
    uint32_t bytes;           // file size incl. header
    bool     active;          // true for this boot's growing file
};
uint8_t flightlogList(FlightInfo* out, uint8_t max);   // oldest-first; count returned

// Header-only read: the file's session_id and recording firmware version
// (from its 64 B header). Returns false if the file is missing/unreadable.
// Core 0 only.
bool flightlogFileInfo(const char* name, uint32_t* sessionOut, char* fwOut, size_t fwLen);

// Read up to `max` records with uptimeMs > sinceMs from file `name`,
// oldest-first — the same cursor contract as GET /history. Returns the count
// filled, or -1 if the file is missing/unreadable. availOut (optional) gets
// the total records qualifying past the cursor, sessionOut (optional) the
// file's session_id. Core 0 only.
int32_t flightlogRead(const char* name, uint32_t sinceMs, HistRecord* out,
                      uint16_t max, uint32_t* availOut, uint32_t* sessionOut);

bool flightlogDelete(const char* name);   // refuses the active file; core 0 only
