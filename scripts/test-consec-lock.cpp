// Host test: consecutive (non-first) loops phase-lock to the first loop's start.
// Models _startRecording (content+phase tied, beat-snapped) + _startEndingRecording
// (floor offset AND content to the L-grid, tied) + playback phase.
// Build: g++ -O2 -std=c++17 scripts/test-consec-lock.cpp -o scripts/test-consec-lock.exe
#include <cstdint>
#include <cstdio>
#include <initializer_list>
typedef uint32_t u32; 
static int g_fails=0;
static void check(const char*n,bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

// _startRecording (CONSECUTIVE, M>0): the latch fires at a beat-grid downbeat
// (masterPhase % gridStep == 0), so anchor DIRECTLY to the latch instant — no
// backdate, no re-snap. recStart = current write head, offset = current masterPhase.
struct Rec { u32 recStart; u32 offset; };
static Rec start_recording_latched(u32 wrNow, u32 latchMasterPhase) {
    return { wrNow, latchMasterPhase };   // no backdate, no snap
}
// First loop (M==0): backdate to the press, sample-true (defines the grid).
static Rec start_recording_first(u32 pressRingBlock, u32 wrNow, u32 masterPhase) {
    return { pressRingBlock, masterPhase - (wrNow - pressRingBlock) };
}
// playback head at a given masterPhase.
static u32 play_block(u32 masterPhase, u32 offset, u32 L) {
    return ((masterPhase - offset) % L + L) % L;
}

int main() {
    const u32 M = 5520;            // phrase (first loop length), masterPhase anchored phase0
    // Consecutive loops of various lengths, recorded starting at arbitrary phases.
    const u32 gridStep = M/16;
    for (u32 L : {M/4, M/2, M, 2*M, 4*M}) {
        // The latch fires _startRecording at a beat downbeat: masterPhase is a
        // gridStep multiple, write head is the true content start. Model that.
        u32 latchPhase = 19 * gridStep;          // some beat downbeat (a gridStep multiple)
        u32 wrNow = 100000;                      // write head at the latch instant
        Rec r = start_recording_latched(wrNow, latchPhase);

        // INVARIANT 1: offset == the latch beat (a gridStep multiple), exactly —
        // no backdate, no re-snap drift.
        check("offset == latch beat (gridStep multiple, no drift)",
              r.offset == latchPhase && (r.offset % gridStep) == 0);

        // INVARIANT 2 (THE FIX): content==phase. recStart is the write head at the
        // latch (clip block 0 audio), offset is the same instant's masterPhase.
        // So play_block at masterPhase==offset is 0 -> reads block 0 == the downbeat
        // the operator played, coinciding with the first-loop start. No late shift.
        check("play_block==0 at offset (coincides with first-loop start)",
              play_block(r.offset, r.offset, L) == 0);
        check("content-start == write head at latch (tied)", r.recStart == wrNow);

        // INVARIANT 3: phase 0 recurs at every L-boundary; offset is a gridStep
        // multiple and L is a multiple of gridStep, so it stays locked to the grid.
        bool locked = true;
        for (u32 k = 0; k < 8; k++)
            if (play_block(r.offset + k*L, r.offset, L) != 0) { locked=false; break; }
        check("phase 0 at every L-boundary (locked to shared grid)", locked);

        printf("   L=%u offset=%u recStart=%u (latch=%u)\n", L, r.offset, r.recStart, latchPhase);
    }

    // First loop (M==0) backdates to the press — sample-true, defines the grid.
    {
        u32 wr=100000, mp=4000, press=wr-40;
        Rec f = start_recording_first(press, wr, mp);
        check("first loop backdated to press (recStart==press)", f.recStart == press);
        check("first loop offset = masterPhase - backBlocks", f.offset == mp - (wr - press));
    }

    if (g_fails){printf("%d FAILURE(S)\n",g_fails);return 1;}
    printf("ALL PASS\n"); return 0;
}
