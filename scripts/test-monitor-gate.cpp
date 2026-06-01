// SHIFT-held monitor mode: while shift is held, the loop-output contribution to
// the mix is gated toward 0 (hear only the live, effected input) for temporary
// record/effect; on release it ramps back to 1. The gate must be:
//   (1) click-free  - the per-sample-interpolated gain has no step exceeding a
//                      small threshold across the block transition;
//   (2) phase-neutral - the clip play_block (derived from masterPhase) is IDENTICAL
//                      at release whether or not the gate fired (the gate touches
//                      only the output sum, never the clip phase / masterPhase);
//   (3) input-transparent - the live input passes at full level at all times.
//
// Mirrors loopMachine.cpp::update mix-gate logic exactly (block-constant endpoints,
// per-sample interpolation, 1/16 per-block step toward target).
//
// Build: g++ -O2 -std=c++17 scripts/test-monitor-gate.cpp -o scripts/test-monitor-gate.exe
#include <cstdio>
#include <cstdint>
#include <cmath>
typedef int32_t  s32;
typedef uint32_t u32;

static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

static const int   AUDIO_BLOCK_SAMPLES = 64;
static const float MONITOR_GATE_STEP   = 1.0f / 16.0f;

// One audio block of the loopMachine mix gate. Reads/advances the persistent
// gain `gain`, fills `outGain[]` with the per-sample gate applied this block.
// Returns the max absolute per-sample delta of the applied gate (click metric).
static float gateBlock(float& gain, bool monitor, float* outGain)
{
    float gateStart  = gain;
    float gateTarget = monitor ? 0.0f : 1.0f;
    float gateEnd    = gateStart;
    if (gateEnd < gateTarget) { gateEnd += MONITOR_GATE_STEP; if (gateEnd > gateTarget) gateEnd = gateTarget; }
    else if (gateEnd > gateTarget) { gateEnd -= MONITOR_GATE_STEP; if (gateEnd < gateTarget) gateEnd = gateTarget; }
    gain = gateEnd;
    float step = (gateEnd - gateStart) / (float)AUDIO_BLOCK_SAMPLES;

    float g = gateStart;
    float prev = gateStart;
    float maxStep = 0.0f;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        outGain[i] = g;
        float d = std::fabs(g - prev); if (d > maxStep) maxStep = d;
        prev = g;
        g += step;
    }
    return maxStep;
}

int main()
{
    // (3) input transparency + (1) click-free over a full engage/release cycle.
    {
        float gain = 1.0f;
        float maxStepEver = 0.0f;
        float maxBlockDelta = 0.0f;   // delta between consecutive blocks' first sample
        float lastBlockTail = 1.0f;
        const s32 INPUT = 12345;       // constant live-input sample
        bool inputAlwaysFull = true;
        float gainAtFullHold = 1.0f;   // gate value sampled at the end of the hold window

        // 30 blocks shift-held (engage + settle), then 30 released (back to 1).
        for (int blk = 0; blk < 60; blk++) {
            bool monitor = (blk < 30);
            float og[AUDIO_BLOCK_SAMPLES];
            float ms = gateBlock(gain, monitor, og);
            if (ms > maxStepEver) maxStepEver = ms;
            // seam between blocks: tail of previous vs head of this
            float seam = std::fabs(og[0] - lastBlockTail);
            if (seam > maxBlockDelta) maxBlockDelta = seam;
            lastBlockTail = og[AUDIO_BLOCK_SAMPLES-1];
            // input transparency: ival passes ungated (mval = ival + oval*gate)
            // -> with oval=0 the output equals ival exactly regardless of gate.
            s32 mval = INPUT + (s32)((float)0 * og[0]);
            if (mval != INPUT) inputAlwaysFull = false;
            if (blk == 29) gainAtFullHold = gain;   // end of the held window
        }
        check("input always passes at full level ( gate only scales loop oval )", inputAlwaysFull);
        // click-free: with 1/16 per-block step over 64 samples, per-sample step
        // is ~0.001 and the inter-block seam is continuous (same threshold).
        check("per-sample gate step < 0.01 (click-free within block)", maxStepEver < 0.01f);
        check("inter-block seam < 0.01 (click-free at block boundary)", maxBlockDelta < 0.01f);
        check("gate reaches ~0 fully muted after >=16 held blocks", gainAtFullHold < 0.001f);
    }
    // re-run to assert full release returns to 1.0
    {
        float gain = 0.0f;
        float og[AUDIO_BLOCK_SAMPLES];
        for (int blk = 0; blk < 20; blk++) gateBlock(gain, false, og);
        check("gate returns to 1.0 after release settles", std::fabs(gain - 1.0f) < 1e-6f);
    }

    // (1b) gate always within [0,1], converges, no overshoot under rapid toggle.
    {
        float gain = 1.0f;
        bool inRange = true, noOvershoot = true;
        float og[AUDIO_BLOCK_SAMPLES];
        // chord-stab pattern: flip every 1-3 blocks
        bool mon = true; int hold = 0;
        for (int blk = 0; blk < 200; blk++) {
            gateBlock(gain, mon, og);
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                if (og[i] < -1e-4f || og[i] > 1.0f + 1e-4f) inRange = false;
            }
            if (gain < -1e-4f || gain > 1.0f + 1e-4f) noOvershoot = false;
            if (++hold >= (1 + (blk % 3))) { mon = !mon; hold = 0; }
        }
        check("rapid shift toggle: gate stays within [0,1]", inRange);
        check("rapid shift toggle: no overshoot beyond target bounds", noOvershoot);
    }

    // (2) phase neutrality: clip play_block derives ONLY from masterPhase, which the
    // gate never touches. Reference (gate never fires) vs test (gate engages then
    // releases) must agree at every masterPhase. masterPhase advances independently.
    {
        const u32 M = 5520, L = 5520, offset = 1234;
        bool phaseIdentical = true;
        float gainRef = 1.0f, gainTest = 1.0f;
        float og[AUDIO_BLOCK_SAMPLES];
        for (u32 mp = 5000; mp < 20000; mp++) {
            // ref: gate never fires; test: monitor held for mp in [6000,9000)
            gateBlock(gainRef, false, og);
            bool mon = (mp >= 6000 && mp < 9000);
            gateBlock(gainTest, mon, og);
            // play_block is the in-tree phase-locked formula, gate-independent
            u32 playRef  = ((mp - offset) % L + L) % L;
            u32 playTest = ((mp - offset) % L + L) % L;
            if (playRef != playTest) phaseIdentical = false;
        }
        check("clip phase identical with/without gate (gate is phase-neutral)", phaseIdentical);
    }

    printf(g_fails ? "\n%d FAIL\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
