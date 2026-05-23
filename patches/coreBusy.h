#ifndef CORE_BUSY_H
#define CORE_BUSY_H

#include <circle/types.h>

// Lock-free per-core busy/idle accounting.
//
// Each core (0..3) updates two u64 counters cooperatively:
//   g_coreBusyTicks[core]  — total clock ticks spent doing useful work
//   g_coreIdleTicks[core]  — total clock ticks spent in WFE / idle waits
//
// Write pattern (cooperative; only own core touches its slot):
//   uint64_t mark = CTimer::GetClockTicks();
//   ... do work ...
//   g_coreBusyTicks[core] += CTimer::GetClockTicks() - mark;
//   mark = CTimer::GetClockTicks();
//   wfe();
//   g_coreIdleTicks[core] += CTimer::GetClockTicks() - mark;
//
// Reader (control plane Core 2) samples snapshots at 0.5Hz and reports
// busy_pct = busy / (busy + idle) over the interval.
//
// 64-bit ticks at 1MHz won't wrap for 584,542 years.

extern volatile u64 g_coreBusyTicks[4];
extern volatile u64 g_coreIdleTicks[4];

#endif
