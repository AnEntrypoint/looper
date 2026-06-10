#ifndef PARAM_SNAPSHOT_H
#define PARAM_SNAPSHOT_H

#include <circle/types.h>
#include <circle/synchronize.h>

// Single-writer (Core 2 control plane) / multi-reader (Core 1 DSP) atomic
// snapshot for params shared from control to DSP. Double-buffered: writer
// fills inactive slot, then atomically flips the active index. Reader
// loads idx with DSB and reads from that slot. Either old-complete or
// new-complete struct is observed — never torn.
//
// All publishes from Core 2 only. Reads from anywhere (Core 1 audio
// path, Core 0 ISR observability).

struct LiveParams {
    bool      liveEngaged;       // mod-wheel + note-on engage
    float     livePitchSemitones;
    float     formantNorm;       // 0..1 via CC
    bool      linkSynced;
    float     linkBPM;
    u32       masterLoopBlocks;  // recomputed when bpm changes
    bool      monitorMode;       // SHIFT held: route loops INTO the effects.
    u8        microRepeatDiv;    // latch-based microrepeat division: 0 = off,
                                 // else 1/2/4/8/16 (beat divisor for the
                                 // beat-repeat/stutter on notes 82..86).
};

extern volatile u32 g_paramActiveIdx;
extern LiveParams   g_paramSlots[2];

inline LiveParams paramSnapshotLoad (void)
{
    DataMemBarrier ();
    u32 idx = g_paramActiveIdx;
    DataMemBarrier ();
    return g_paramSlots[idx & 1];
}

inline void paramSnapshotPublish (const LiveParams &p)
{
    u32 cur = g_paramActiveIdx;
    u32 nxt = (cur ^ 1u) & 1u;
    g_paramSlots[nxt] = p;
    DataMemBarrier ();
    g_paramActiveIdx = nxt;
    DataMemBarrier ();
    asm volatile ("sev" ::: "memory");
}

#endif
