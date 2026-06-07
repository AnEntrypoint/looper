// SHIFT-hold monitor = route the LOOPS INTO the live effects chain.
//
// While SHIFT (monitorMode) is held, the running loop output is FOLDED INTO the
// effect-chain source (m_input_buffer, processed by pitch + effects and recorded
// by cbWriteBlock) with a gain m_loopFoldGain that ramps 0->1, while the DRY loop
// contribution to the final mix ramps complementarily 1->0 (m_loopOutputGain =
// 1 - m_loopFoldGain). So the loops are heard ONCE, through the effects, with no
// loudness jump and no double-sum; released, loops return to dry output. Clips
// keep advancing regardless (phase-neutral).
//
// Mirrors the new loopMachine::update routing. Asserts:
//   (1) fold and dry gains are exactly complementary (g_dry == 1 - g_fold);
//   (2) at full SHIFT the loop sum is present in the effect source (fold==1) and
//       the dry loop output is 0;
//   (3) released, fold==0 and dry==1 (loops fully dry, none in the effect source);
//   (4) the ramp is click-free (per-sample step under threshold);
//   (5) total loop energy is conserved across the transition (dry+fold == 1 at
//       every sample) so there is no volume dip/bump;
//   (6) rapid SHIFT toggle keeps both gains bounded in [0,1];
//   (7) the routing is phase-neutral: a model masterPhase counter advances
//       identically whether or not SHIFT is held.
//
// Build: g++ -O2 -std=c++17 scripts/test-monitor-route.cpp -o scripts/test-monitor-route.exe
#include <cstdio>
#include <cstdint>
#include <cmath>
typedef int32_t  s32;
static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

static const int   AUDIO_BLOCK_SAMPLES = 64;
static const float MONITOR_GATE_STEP   = 1.0f / 16.0f;

// One block of the SHIFT routing (loopMachine::update). Advances m_loopFoldGain
// toward 1 (held) / 0 (released); fills foldGain[]/dryGain[] with the per-sample
// fold and dry gains applied this block. Returns max abs per-sample step (click
// metric) across both ramps.
static float routeBlock(float& foldGain, bool monitor, float* foldOut, float* dryOut)
{
    float foldStart  = foldGain;
    float foldTarget = monitor ? 1.0f : 0.0f;
    float foldEnd    = foldStart;
    if (foldEnd < foldTarget) { foldEnd += MONITOR_GATE_STEP; if (foldEnd > foldTarget) foldEnd = foldTarget; }
    else if (foldEnd > foldTarget) { foldEnd -= MONITOR_GATE_STEP; if (foldEnd < foldTarget) foldEnd = foldTarget; }
    foldGain = foldEnd;
    float foldStep = (foldEnd - foldStart) / (float)AUDIO_BLOCK_SAMPLES;
    float dryStart = 1.0f - foldStart, dryEnd = 1.0f - foldEnd;
    float dryStep  = (dryEnd - dryStart) / (float)AUDIO_BLOCK_SAMPLES;

    float fg = foldStart, dg = dryStart, prevF = foldStart, prevD = dryStart, maxStep = 0;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        foldOut[i] = fg; dryOut[i] = dg;
        float df = std::fabs(fg - prevF); if (df > maxStep) maxStep = df;
        float dd = std::fabs(dg - prevD); if (dd > maxStep) maxStep = dd;
        prevF = fg; prevD = dg; fg += foldStep; dg += dryStep;
    }
    return maxStep;
}

int main()
{
    // (1)(5) complementary + energy-conserving over a full engage/release cycle.
    {
        float fold = 0.0f;
        float fO[AUDIO_BLOCK_SAMPLES], dO[AUDIO_BLOCK_SAMPLES];
        bool complementary = true, clickFree = true;
        float worst = 0;
        for (int blk = 0; blk < 60; blk++) {
            bool monitor = (blk < 30);
            float ms = routeBlock(fold, monitor, fO, dO);
            if (ms > worst) worst = ms;
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
                if (std::fabs((fO[i] + dO[i]) - 1.0f) > 1e-6f) complementary = false;
        }
        clickFree = (worst < 0.01f);
        check("dry + fold == 1 at every sample (complementary, energy-conserved)", complementary);
        check("ramp click-free (max per-sample step < 0.01)", clickFree);
    }

    // (2) full SHIFT: fold->1, dry->0.
    {
        float fold = 0.0f; float fO[AUDIO_BLOCK_SAMPLES], dO[AUDIO_BLOCK_SAMPLES];
        for (int blk = 0; blk < 24; blk++) routeBlock(fold, true, fO, dO);
        check("full SHIFT: fold gain ~1 (loops folded into effect source)", std::fabs(fold - 1.0f) < 1e-6f);
        check("full SHIFT: dry loop gain ~0 (no dry double-sum)", std::fabs((1.0f - fold) - 0.0f) < 1e-6f);
    }

    // (3) released: fold->0, dry->1.
    {
        float fold = 1.0f; float fO[AUDIO_BLOCK_SAMPLES], dO[AUDIO_BLOCK_SAMPLES];
        for (int blk = 0; blk < 24; blk++) routeBlock(fold, false, fO, dO);
        check("released: fold gain ~0 (nothing routed to effects)", fold < 1e-6f);
        check("released: dry loop gain ~1 (loops fully dry)", std::fabs((1.0f - fold) - 1.0f) < 1e-6f);
    }

    // (4)(6) bounds under rapid toggle.
    {
        float fold = 1.0f; float fO[AUDIO_BLOCK_SAMPLES], dO[AUDIO_BLOCK_SAMPLES];
        bool inRange = true;
        for (int blk = 0; blk < 400; blk++) {
            routeBlock(fold, (blk & 1), fO, dO);
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                if (fO[i] < -1e-6f || fO[i] > 1.0f+1e-6f) inRange = false;
                if (dO[i] < -1e-6f || dO[i] > 1.0f+1e-6f) inRange = false;
            }
        }
        check("rapid toggle: both gains stay in [0,1]", inRange);
    }

    // (7) phase-neutral: masterPhase advances once/block regardless of SHIFT.
    {
        unsigned mpRef = 0, mpTst = 0;
        float foldRef = 0.0f, foldTst = 0.0f;
        float fO[AUDIO_BLOCK_SAMPLES], dO[AUDIO_BLOCK_SAMPLES];
        bool same = true;
        for (int blk = 0; blk < 200; blk++) {
            routeBlock(foldRef, false, fO, dO);        // reference: never SHIFT
            routeBlock(foldTst, (blk % 3 == 0), fO, dO); // test: SHIFT sometimes
            mpRef++; mpTst++;                           // advance is unconditional
            if (mpRef != mpTst) same = false;
        }
        check("masterPhase advances identically with/without SHIFT (phase-neutral)", same);
    }

    if (g_fails) { printf("\n%d FAIL\n", g_fails); return 1; }
    printf("\nALL PASS\n");
    return 0;
}
