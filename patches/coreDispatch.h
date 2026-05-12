#ifndef CORE_DISPATCH_H
#define CORE_DISPATCH_H

#include <circle/types.h>

// SPSC dispatch ring + WFE/SEV wakeup.
//
// Producers (USB IN/OUT completion ISRs on Core 0, watchdog on Core 2)
// push a small token (DISPATCH_AUDIO) and issue SEV. Consumer (Core 1
// DSP worker) waits in WFE, then drains.
//
// 64 slots * 4 bytes = 256 B static. Allocation-free, ISR-safe.
// Identical pattern to audioTelemetry but smaller payload.

enum DispatchCode : u32 {
    DISPATCH_AUDIO        = 1,   // run AudioSystem::doUpdate once
    DISPATCH_CONTROL_TICK = 2,   // optional Core-2 wakeup
};

void coreDispatchPush  (u32 code);          // ISR-safe, with DSB+SEV
bool coreDispatchPop   (u32 *outCode);      // returns false when empty
void coreDispatchWait  (void);              // WFE if empty

extern volatile unsigned g_dispatchDropped;

#endif
