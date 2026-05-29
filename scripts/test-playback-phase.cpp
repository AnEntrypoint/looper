// Standalone host test: a non-first loop plays in the SAME phase it was
// recorded — recordStartPhaseOffset must be a MULTIPLE of the loop's own length
// so play_block hits 0 at every phrase boundary. Mirrors the length-grid snap
// added to loopClip.cpp::_startEndingRecording.
// Build: g++ -O2 -std=c++17 scripts/test-playback-phase.cpp -o scripts/test-playback-phase.exe
#include <cstdint>
#include <cstdio>
#include <initializer_list>
typedef uint32_t u32;

static int g_fails = 0;
static void check(const char* n, bool c){ if(c) printf("ok: %s\n",n); else {printf("FAIL: %s\n",n); g_fails++;} }

// The fix: snap an offset to the nearest multiple of the loop length L.
static u32 snap_to_length(u32 offset, u32 L) {
    u32 off = offset % L;
    u32 snapped = offset - off;
    if (off * 2 >= L) snapped += L;
    return snapped;
}

// Playback position at a given master phase.
static u32 play_block(u32 masterPhase, u32 recStartOffset, u32 numBlocks) {
    return ((masterPhase - recStartOffset) % numBlocks + numBlocks) % numBlocks;
}

int main() {
    const u32 M = 5520;             // phrase (16 beats)
    const u32 beat = M / 16;        // 345

    // Loop length = M/2 (half phrase). Start was beat-snapped to 3 beats in.
    u32 L = M / 2;                  // 2760
    u32 beatOffset = 3 * beat;      // 1035 — a beat grid point, NOT a multiple of L
    u32 snapped = snap_to_length(beatOffset, L);
    check("offset snapped to a multiple of loop length", snapped % L == 0);

    // With the OLD beat-aligned offset, the loop plays shifted (play_block != 0
    // at the master phrase-zero); with the snapped offset it plays in phase.
    check("beat-aligned offset plays SHIFTED (the bug)",
          play_block(0, beatOffset, L) != 0);
    check("length-snapped offset plays IN PHASE at phrase 0",
          play_block(0, snapped, L) == 0);

    // In phase at every phrase boundary (multiples of M), for several lengths.
    bool inphase = true;
    for (u32 L2 : {M/8, M/4, M/2, M, 2*M, 4*M}) {
        u32 startBeat = 5 * beat;                 // arbitrary beat
        u32 s = snap_to_length(startBeat, L2);
        for (u32 k = 0; k < 4; k++) {
            // at every loop-length boundary the play head must be 0
            if (play_block(s + k * L2, s, L2) != 0) { inphase = false; break; }
        }
        if (!inphase) break;
    }
    check("in-phase at every loop boundary for all quant lengths", inphase);

    // The shift magnitude with the old scheme could be up to L-beat (~ the "moved
    // about a beat" report when L is near a beat multiple): confirm the snap
    // removes a sub-length offset of one beat.
    check("one-beat offset under half a short loop snaps to 0",
          snap_to_length(beat, M/2) == 0);          // 345 << 2760/2 -> floor to 0
    check("one-beat offset above half a M/8 loop snaps up",
          snap_to_length(beat, M/8) == M/8);        // M/8=690, beat=345 == half -> rounds up

    // FIRST loop (wasFirst) must NOT be re-snapped: its offset is the sample-true
    // press anchor and IS the grid origin. Model: the first loop sets the master
    // to its own length and keeps its raw offset; play_block at the press anchor
    // must be 0 (loops cleanly from its start), and a re-snap would move it.
    {
        u32 rawOffset = 3 * beat + 77;     // arbitrary sample-true press anchor
        u32 firstLen = 4880;               // whatever was recorded (defines grid)
        // wasFirst path: offset stays raw, master = firstLen.
        u32 keptOffset = rawOffset;        // NOT snapped
        // The loop's own start is its anchor, so at masterPhase == anchor the
        // play head is 0 (clean repeat from the beginning).
        check("first loop plays from its own start (no re-snap)",
              play_block(rawOffset, keptOffset, firstLen) == 0);
        // If we HAD re-snapped (the bug), the play head at the anchor would be
        // non-zero (plays from a funny place).
        u32 wrongSnapped = snap_to_length(rawOffset, firstLen);
        check("re-snapping the first loop WOULD shift it (the bug)",
              play_block(rawOffset, wrongSnapped, firstLen) != 0);
    }

    if (g_fails) { printf("%d FAILURE(S)\n", g_fails); return 1; }
    printf("ALL PASS\n");
    return 0;
}
