#include "coreDispatch.h"
#include <circle/synchronize.h>

#define DISPATCH_RING_SIZE 64
#define DISPATCH_RING_MASK (DISPATCH_RING_SIZE - 1)

static u32 s_ring[DISPATCH_RING_SIZE];
static volatile unsigned s_wr = 0;
static volatile unsigned s_rd = 0;

volatile unsigned g_dispatchDropped = 0;

void coreDispatchPush (u32 code)
{
    unsigned wr = s_wr;
    unsigned rd = s_rd;
    if ((wr - rd) >= DISPATCH_RING_SIZE) {
        g_dispatchDropped++;
        return;
    }
    s_ring[wr & DISPATCH_RING_MASK] = code;
    DataMemBarrier ();
    s_wr = wr + 1;
    DataMemBarrier ();
    asm volatile ("sev" ::: "memory");
}

bool coreDispatchPop (u32 *outCode)
{
    unsigned rd = s_rd;
    DataMemBarrier ();
    if (rd == s_wr) return false;
    *outCode = s_ring[rd & DISPATCH_RING_MASK];
    s_rd = rd + 1;
    return true;
}

void coreDispatchWait (void)
{
    if (s_rd == s_wr) {
        asm volatile ("wfe" ::: "memory");
    }
}
