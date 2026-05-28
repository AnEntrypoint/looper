// continuousBuffer.cpp — see continuousBuffer.h. Single writer (Core 1 audio
// update); the write is a plain memcpy + monotonic counter advance, no locks.
#include "Looper.h"
#include "continuousBuffer.h"
#include <circle/util.h>
#include <circle/timer.h>
#include <circle/alloc.h>
#include <circle/logger.h>

s16          *g_cbBuffer = 0;
volatile u32 g_cbWriteBlock = 0;

void cbInit(void)
{
    if (g_cbBuffer) return;
    size_t bytes = (size_t)CB_RING_BLOCKS * AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS * sizeof(s16);
    g_cbBuffer = (s16 *)malloc(bytes);
    if (g_cbBuffer) memset(g_cbBuffer, 0, bytes);
    CLogger::Get()->Write("cbuffer", LogNotice, "continuous buffer %u bytes @0x%08X", (unsigned)bytes, (u32)g_cbBuffer);
}

volatile u32 g_cbLastBackdateSamples = 0;
volatile u32 g_cbLastPressLatencyUs  = 0;
volatile u32 g_cbLastBackdateClamped = 0;
volatile unsigned g_pendingPressTicks = 0;

void cbWriteBlock(const s32 *in)
{
    if (!g_cbBuffer) return;             // not yet allocated (pre-setup safety)
    s16 *dst = cbBlockPtr(g_cbWriteBlock);
    // in[] is the loopMachine interleaved s32 input block (L,R per frame).
    const int n = AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS;
    for (int i = 0; i < n; i++)
    {
        s32 v = in[i];
        dst[i] = v > 32767 ? 32767 : (v < -32768 ? -32768 : (s16)v);
    }
    DataMemBarrier();
    g_cbWriteBlock++;
}

void cbCopyRange(u32 startBlock, u32 stopBlock, s16 *dst)
{
    const int blkSamples = AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS;
    for (u32 b = startBlock; b != stopBlock; b++)
    {
        memcpy(dst, cbBlockPtr(b), blkSamples * sizeof(s16));
        dst += blkSamples;
    }
}

u32 cbBackdatedBlock(unsigned press_ticks)
{
    u32 wr = g_cbWriteBlock;                 // current write head (next-to-write)

    // backdate in samples = (press->now elapsed) + fixed ring/ADC lag.
    u32 backSamples = CB_FIXED_LAG_SAMPLES;
    if (press_ticks != 0)                    // 0 = no timestamp -> fixed lag only
    {
        u32 now = CTimer::GetClockTicks();
        u32 elapsedUs = now - press_ticks;   // wrap-safe unsigned modular diff
        g_cbLastPressLatencyUs = elapsedUs;
        // us -> samples at the internal rate. 64-bit to avoid overflow.
        u32 elapsedSamples = (u32)(((u64)elapsedUs * (u64)AUDIO_SAMPLE_RATE) / 1000000ull);
        backSamples += elapsedSamples;
    }
    else g_cbLastPressLatencyUs = 0;

    u32 backBlocks = backSamples / AUDIO_BLOCK_SAMPLES;

    // Clamp to the live horizon so the returned block is always readable
    // (never crosses the ~3-min wrap into overwritten audio).
    u32 avail = cbBlocksAvailable();
    if (avail > 0) avail -= 1;               // keep at least the current block valid
    if (backBlocks > avail) { backBlocks = avail; g_cbLastBackdateClamped = 1; }
    else                      g_cbLastBackdateClamped = 0;

    g_cbLastBackdateSamples = backBlocks * AUDIO_BLOCK_SAMPLES;
    return wr - backBlocks;                  // absolute press-instant block
}
