#include "audioTelemetry.h"
#include <circle/timer.h>
#include <circle/synchronize.h>

#define TELEM_RING_SIZE 256
#define TELEM_RING_MASK (TELEM_RING_SIZE - 1)

static AudioTelemEvent  s_ring[TELEM_RING_SIZE];
static volatile unsigned s_wr = 0;
static volatile unsigned s_rd = 0;

volatile unsigned g_telemDropped = 0;

void audioTelemetryPush (u32 code, u32 arg)
{
    unsigned wr = s_wr;
    unsigned rd = s_rd;
    if ((wr - rd) >= TELEM_RING_SIZE) {
        g_telemDropped++;
        return;
    }
    AudioTelemEvent *e = &s_ring[wr & TELEM_RING_MASK];
    e->code  = code;
    e->ticks = CTimer::GetClockTicks ();
    e->arg   = arg;
    DataMemBarrier ();
    s_wr = wr + 1;
}

bool audioTelemetryPop (AudioTelemEvent *out)
{
    unsigned rd = s_rd;
    DataMemBarrier ();
    if (rd == s_wr) return false;
    *out = s_ring[rd & TELEM_RING_MASK];
    s_rd = rd + 1;
    return true;
}
