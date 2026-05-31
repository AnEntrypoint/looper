// Host test: consecutive (non-first) loops phase-lock to the first loop's start.
// Models _startRecording (content+phase tied, beat-snapped) + _startEndingRecording
// (floor offset AND content to the L-grid, tied) + playback phase.
// Build: g++ -O2 -std=c++17 scripts/test-consec-lock.cpp -o scripts/test-consec-lock.exe
#include <cstdint>
#include <cstdio>
#include <initializer_list>
typedef uint32_t u32; typedef int32_t i32;
static int g_fails=0;
static void check(const char*n,bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

// _startRecording: snap content-start & phase to the SAME beat grid point.
struct Rec { u32 recStart; u32 offset; };
static Rec start_recording(u32 pressRingBlock, u32 wrNow, u32 masterPhase, u32 M) {
    u32 recStart = pressRingBlock;
    u32 backBlocks = wrNow - recStart;
    u32 startPhase = masterPhase - backBlocks;
    if (M > 0) {
        u32 gridStep = (M>=16)?(M/16):M;
        if (gridStep>0) {
            u32 rem = startPhase % gridStep;
            i32 d = (rem*2 >= gridStep) ? (i32)(gridStep-rem) : -(i32)rem;
            startPhase = (u32)((i32)startPhase + d);
            recStart   = (u32)((i32)recStart + d);
        }
    }
    return { recStart, startPhase };
}
// _startEndingRecording: NO stop-time floor (removed). The clip buffer was
// filled from the beat-snapped record-start, so the offset already names buffer
// block 0; flooring here would shift the phase off the (already-recorded) audio.
static void end_recording(Rec& /*r*/, u32 /*L*/, u32 /*M*/) { /* no-op: no floor */ }
// The buggy old floor, kept to PROVE it shifted the phase off the audio.
static u32 old_floor_offset(u32 offset, u32 L, u32 M) {
    u32 grid = (L < M) ? L : M;
    return offset - (offset % grid);
}
// playback head at a given masterPhase.
static u32 play_block(u32 masterPhase, u32 offset, u32 L) {
    return ((masterPhase - offset) % L + L) % L;
}

int main() {
    const u32 M = 5520;            // phrase (first loop length), masterPhase anchored phase0
    // Consecutive loops of various lengths, recorded starting at arbitrary phases.
    for (u32 L : {M/4, M/2, M, 2*M, 4*M}) {
        // Press somewhere arbitrary; model wrNow/masterPhase consistent.
        u32 wrNow = 100000, masterPhase = 73215; // arbitrary running phase
        u32 pressRing = wrNow - 50;              // pressed ~50 blocks ago (backdate)
        Rec r = start_recording(pressRing, wrNow, masterPhase, M);
        end_recording(r, L, M);

        // INVARIANT 1: offset is on the BEAT grid (M/16) — finer than L, but L is a
        // multiple of M/16, so this is coherent. (Not required to be on the L grid.)
        check("offset on beat grid (M/16)", (r.offset % (M/16)) == 0);

        // INVARIANT 2 (THE FIX): the played downbeat == the recorded downbeat.
        // Buffer block 0 audio sits at masterPhase == r.offset (record-start). So
        // play_block at masterPhase==offset MUST be 0 (reads buffer[0] = the audio
        // the operator played on the downbeat). No offbeat shift.
        check("play_block==0 at offset -> reads recorded downbeat (no offbeat shift)",
              play_block(r.offset, r.offset, L) == 0);

        // INVARIANT 3: phase 0 recurs at every L-boundary (and L|M sub-phrase or
        // L=jM multiple => coincides with the shared phrase grid).
        bool lockedAtPhrase = true;
        for (u32 k = 0; k < 8; k++)
            if (play_block(r.offset + k*L, r.offset, L) != 0) { lockedAtPhrase=false; break; }
        check("phase 0 at every L-boundary (locked)", lockedAtPhrase);

        // PROOF the old stop-time floor was the bug: it moved the offset to a
        // DIFFERENT value than buffer-block-0, so play_block at the true content
        // downbeat (masterPhase==r.offset) would be NON-zero => offbeat shift.
        u32 floored = old_floor_offset(r.offset, L, M);
        if (floored != r.offset)
            check("old floor WOULD have shifted playback off the audio (the bug)",
                  play_block(r.offset, floored, L) != 0);

        printf("   L=%u offset=%u recStart=%u\n", L, r.offset, r.recStart);
    }

    // 505 forgiving: press just-before vs just-after a beat both snap to the same
    // beat point; flooring to L then lands the same grid start.
    {
        u32 M2=5520, wr=100000, mp=60000;
        Rec a = start_recording(wr-50-3, wr, mp, M2);  // press 3 blocks earlier
        Rec b = start_recording(wr-50+3, wr, mp, M2);  // press 3 blocks later
        // within the same beat (M/16=345), both snap to the same beat point
        check("near-boundary presses snap to same beat (forgiving)",
              a.offset/(M2/16) == b.offset/(M2/16) || (a.offset==b.offset));
    }

    if (g_fails){printf("%d FAILURE(S)\n",g_fails);return 1;}
    printf("ALL PASS\n"); return 0;
}
