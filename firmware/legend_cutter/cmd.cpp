// cmd.cpp — see cmd.h.
//
// Classic lock-free SPSC ring: head is written ONLY by the producer (network
// task), tail ONLY by the consumer (control loop). Each side only reads the
// other's index. 16-bit aligned index loads/stores are atomic on the ESP32; the
// full memory barrier orders the payload write/read relative to the index
// publish so the consumer never reads a slot before its payload is visible.

#include "cmd.h"
#include "config.h"

static Command           ring[CMD_QUEUE_LEN];
static volatile uint16_t qHead = 0;   // producer writes, consumer reads
static volatile uint16_t qTail = 0;   // consumer writes, producer reads

bool cmdEnqueue(const Command& c) {
    uint16_t h    = qHead;
    uint16_t next = (uint16_t)((h + 1) % CMD_QUEUE_LEN);
    if (next == qTail) return false;        // full → drop, never block
    ring[h] = c;                            // write payload first…
    __sync_synchronize();                   // …then make it visible before publish
    qHead = next;                           // publish
    return true;
}

bool cmdTryDequeue(Command& out) {
    if (qTail == qHead) return false;       // empty → instant return, never block
    __sync_synchronize();                   // read payload written before head publish
    out = ring[qTail];
    __sync_synchronize();
    qTail = (uint16_t)((qTail + 1) % CMD_QUEUE_LEN);
    return true;
}
