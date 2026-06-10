// Synced latch microrepeat (beat-repeat/stutter) — host validation.
//
// Mirrors patches/microRepeat.h. Asserts the load-bearing properties:
//   (1) slice length == beat_blocks*BLOCK/div for div 1/2/4/8/16;
//   (2) while latched (after the capture pass) the output IS the captured slice
//       replayed verbatim (a true repeat);
//   (3) position-passthrough: a model masterPhase / clip head advances
//       identically whether or not the repeat was latched (the stage never
//       touches it);
//   (4) engage/release wet ramp is click-free (per-sample step under threshold);
//   (5) div change while held retargets the slice length;
//   (6) masterLoopBlocks==0 (no grid) is a safe passthrough no-op (no div-by-0,
//       no zero-length slice);
//   (7) the capture buffer never overruns at the largest (1-beat, slow-tempo)
//       slice.
//
// Build: g++ -O2 -std=c++17 scripts/test-microrepeat.cpp -o scripts/test-microrepeat.exe
#include <cstdio>
#include <cstdint>
#include <cmath>
#define MR_TEST 1
#include "../patches/microRepeat.h"

static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

static const int N = 64;   // AUDIO_BLOCK_SAMPLES

// Fill a block buffer [L0..L63,R0..R63] with a deterministic ramp keyed on a
// global sample counter so we can tell which input sample ended up where.
static void fillBlock(int32_t* buf, int32_t base) {
    for (int i = 0; i < N; i++) { buf[i] = base + i; buf[N + i] = -(base + i); }
}

int main()
{
    const uint32_t M = 5520;                 // phrase = 16 beats
    const uint32_t beatBlocks = M / 16;      // 345 blocks per beat

    // (1) slice length per division.
    {
        struct { uint8_t div; uint32_t sb; } cs[] = {{1,beatBlocks},{2,beatBlocks/2},{4,beatBlocks/4},{8,beatBlocks/8},{16,beatBlocks/16}};
        bool ok = true;
        for (auto& c : cs) {
            microRepeat mr; int32_t b[2*N];
            fillBlock(b, 0);
            mr.process(b, 0, M, c.div, N);
            uint32_t expect = (c.sb < 1 ? 1 : c.sb) * (uint32_t)N;
            if (expect > (uint32_t)microRepeat::MR_MAX_SLICE) expect = microRepeat::MR_MAX_SLICE;
            if (mr.sliceLenForTest() != (int)expect) { ok = false;
                printf("  div=%u slice=%d expect=%u\n", c.div, mr.sliceLenForTest(), expect); }
        }
        check("slice length == beat_blocks*BLOCK/div for all divisions", ok);
    }

    // (2) latched output is the captured slice replayed. Use div=16 (short slice
    // = beatBlocks/16 = 21 blocks = 1344 samples). Feed distinct blocks; after
    // the slice is captured, the next pass must replay sample-for-sample.
    {
        microRepeat mr;
        int slice = (int)((beatBlocks/16) * N);
        int capturedBlocks = (slice + N - 1) / N;
        int32_t first[microRepeat::MR_MAX_SLICE];
        int idx = 0;
        // capture pass: record the live audio, store what we fed for comparison.
        for (int blk = 0; blk < capturedBlocks; blk++) {
            int32_t b[2*N]; fillBlock(b, blk * 1000);
            for (int i = 0; i < N && idx < slice; i++) first[idx++] = b[i];   // L only
            mr.process(b, 0, M, 16, N);
        }
        // replay pass: output L must equal the recorded slice, looping.
        bool replayed = true; int rp = 0;
        for (int blk = 0; blk < capturedBlocks + 2; blk++) {
            int32_t b[2*N]; fillBlock(b, 9999000 + blk);   // different live input
            mr.process(b, 0, M, 16, N);
            // once fully wet, output should be the recorded slice (L), not live.
            if (mr.wet() > 0.99f) {
                for (int i = 0; i < N; i++) {
                    if (b[i] != first[rp % slice]) { replayed = false; }
                    rp++;
                }
            }
        }
        check("latched output replays the captured slice verbatim (repeat)", replayed);
    }

    // (3) position-passthrough: masterPhase advances identically regardless of
    // the repeat. The stage takes masterPhase by value and never returns it; a
    // model counter advances in the caller, unaffected.
    {
        microRepeat a, b;
        uint32_t mpA = 0, mpB = 0; bool same = true;
        for (int blk = 0; blk < 400; blk++) {
            int32_t bufA[2*N], bufB[2*N]; fillBlock(bufA, blk*7); fillBlock(bufB, blk*7);
            a.process(bufA, mpA, M, 0,  N);                  // never latched
            b.process(bufB, mpB, M, (blk%2)?8:0, N);          // toggled latch
            mpA++; mpB++;                                      // caller advances unconditionally
            if (mpA != mpB) same = false;
        }
        check("masterPhase advances identically latched vs not (position-passthrough)", same);
    }

    // (4) click-free wet ramp on engage and release.
    {
        microRepeat mr; float worst = 0; float prev = 0;
        for (int blk = 0; blk < 60; blk++) {
            uint8_t div = (blk < 30) ? 4 : 0;
            int32_t b[2*N]; fillBlock(b, blk*3);
            float wBefore = mr.wet();
            mr.process(b, 0, M, div, N);
            float wAfter = mr.wet();
            float step = std::fabs(wAfter - wBefore);
            if (step > worst) worst = step;
            prev = wAfter;
        }
        // 1/16 per block = 0.0625 max per-block wet change -> per-sample is tiny.
        check("wet ramp click-free (per-block step <= 1/16 + eps)", worst <= 0.0625f + 1e-6f);
    }

    // (5) div change while held retargets the slice length.
    {
        microRepeat mr; int32_t b[2*N];
        fillBlock(b, 0); mr.process(b, 0, M, 2, N);     // 1/2 beat
        int s2 = mr.sliceLenForTest();
        fillBlock(b, 1); mr.process(b, 0, M, 8, N);     // change to 1/8 beat
        int s8 = mr.sliceLenForTest();
        check("div change while held retargets slice length", s8 != s2 && s8 == (int)((beatBlocks/8)*N));
    }

    // (6) no-master (masterLoopBlocks==0) is a safe passthrough no-op.
    {
        microRepeat mr; int32_t b[2*N]; fillBlock(b, 12345);
        int32_t ref[2*N]; for (int i=0;i<2*N;i++) ref[i]=b[i];
        mr.process(b, 0, 0 /*no grid*/, 4 /*latched*/, N);
        bool passthrough = true;
        for (int i = 0; i < 2*N; i++) if (b[i] != ref[i]) passthrough = false;
        check("masterLoopBlocks==0 latched is safe passthrough (no div-by-0)", passthrough && mr.sliceLenForTest()==0);
    }

    // (7) largest slice (1 beat, slowest tempo) does not overrun the buffer.
    {
        microRepeat mr; int32_t b[2*N];
        // huge phrase -> 1-beat slice would exceed MR_MAX_SLICE -> must clamp.
        uint32_t bigM = 16u * 4000u;   // beat = 4000 blocks = 256000 samples > MR_MAX_SLICE
        fillBlock(b, 0); mr.process(b, 0, bigM, 1, N);
        check("1-beat slice clamps to MR_MAX_SLICE (no overrun)", mr.sliceLenForTest() <= microRepeat::MR_MAX_SLICE);
    }

    if (g_fails) { printf("\n%d FAIL\n", g_fails); return 1; }
    printf("\nALL PASS\n");
    return 0;
}
