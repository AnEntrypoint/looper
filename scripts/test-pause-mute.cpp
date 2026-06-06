// Pause = MUTE: a paused loop keeps advancing its play head (phase-locked to
// the master) and only its OUTPUT is gated to silence. So pause/resume never
// changes position, rapid mute/unmute is instant, and once a loop is recorded
// it never stops playing the same way it started.
//
// Mirrors the NEW loopClipUpdate.cpp model: the play-head advance runs every
// block regardless of m_paused (it is outside the output guard), and a
// click-free per-sample m_pauseGain ramp (1/16 per block) gates the output.
//
// Asserts:
//   (1) a paused clip's play_block tracks a never-paused reference EXACTLY, at
//       every block, for L<M, L==M, L>M (head keeps advancing while muted);
//   (2) resume is position-identical for ANY pause duration (zero drift);
//   (3) output gain reaches ~0 while paused and ~1 while playing;
//   (4) the gain ramp has no per-sample step exceeding a click threshold;
//   (5) rapid mute/unmute leaves position invariant and gain in [0,1].
//
// Build: g++ -O2 -std=c++17 scripts/test-pause-mute.cpp -o scripts/test-pause-mute.exe
#include <cstdio>
#include <cstdint>
#include <cmath>
typedef uint32_t u32;
static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

static const int    AUDIO_BLOCK_SAMPLES = 64;
static const double PAUSE_GAIN_STEP     = 1.0 / 16.0;

struct Clip {
    u32 L;        // num_blocks
    u32 offset;   // recordStartPhaseOffset
    u32 play;     // play_block
    bool paused;
    double pauseGain;
};

// NEW play-head advance (loopClipUpdate.cpp): runs UNCONDITIONALLY each block,
// independent of m_paused. L<=M phase-locked to masterPhase; L>M self-advance.
static void advance(Clip& c, u32 masterPhase, u32 M) {
    if (M > 0 && c.L <= M)
        c.play = ((masterPhase - c.offset) % c.L + c.L) % c.L;
    else { c.play++; if (c.play >= c.L) c.play = 0; }
}

// One block of the output pause-gain ramp (loopClipUpdate.cpp). Fills outGain[]
// with the per-sample applied gain; returns the max abs per-sample delta (click
// metric).
static double pauseBlock(Clip& c, double* outGain) {
    double pgStart  = c.pauseGain;
    double pgTarget = c.paused ? 0.0 : 1.0;
    double pgEnd    = pgStart;
    if (pgEnd < pgTarget) { pgEnd += PAUSE_GAIN_STEP; if (pgEnd > pgTarget) pgEnd = pgTarget; }
    else if (pgEnd > pgTarget) { pgEnd -= PAUSE_GAIN_STEP; if (pgEnd < pgTarget) pgEnd = pgTarget; }
    c.pauseGain = pgEnd;
    double step = (pgEnd - pgStart) / (double)AUDIO_BLOCK_SAMPLES;
    double pg = pgStart, maxStep = 0, prev = pgStart;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        outGain[i] = pg;
        double d = std::fabs(pg - prev); if (d > maxStep) maxStep = d;
        prev = pg; pg += step;
    }
    return maxStep;
}

// (1)+(2): paused clip's position tracks the never-paused reference exactly,
// across the whole pause window and after resume.
static bool simPosition(u32 M, u32 L, u32 offset, u32 pausePhase, u32 resumePhase, const char* label) {
    Clip ref{L, offset, 0, false, 1.0};
    Clip tst{L, offset, 0, false, 1.0};
    u32 mp = offset;                         // start at recorded downbeat
    advance(ref, mp, M); advance(tst, mp, M);
    bool ok = true;
    for (u32 k = 0; k < 5*M + 13; k++) {
        // toggle the test clip's pause latch over [pausePhase, resumePhase)
        tst.paused = (mp >= pausePhase && mp < resumePhase);
        mp++;
        advance(ref, mp, M);                 // reference never pauses
        advance(tst, mp, M);                 // test advances even while paused
        if (ref.play != tst.play) { ok = false;
            printf("  [%s] DRIFT at mp=%u ref=%u tst=%u\n", label, mp, ref.play, tst.play); break; }
    }
    if (ok) printf("  [%s] M=%u L=%u off=%u: position-identical through pause+resume\n", label, M, L, offset);
    return ok;
}

int main(){
    const u32 M = 5520;

    // (1)+(2) position invariance for the three regimes, arbitrary pause windows.
    check("sub-phrase L=M/4 paused head tracks reference",
          simPosition(M, M/4, 0, M/8, M/4 + 37, "subphrase"));
    check("first-loop L==M off=0 paused head tracks reference",
          simPosition(M, M, 0, M/3, M + 91, "firstloop-off0"));
    check("first-loop L==M off=1234 paused head tracks reference",
          simPosition(M, M, 1234, M/3, M + 91, "firstloop-offNZ"));
    check("phrase-or-longer L=2M paused head tracks reference",
          simPosition(M, 2*M, 0, M/2, M + 200, "L2M"));

    // long pause: 10 phrases muted, still position-identical on resume.
    check("10-phrase pause stays position-identical (zero drift)",
          simPosition(M, M/2, 0, M, 11*M, "longpause"));

    // (3)+(4) output gain reaches 0 paused / 1 playing, click-free ramp.
    {
        Clip c{M, 0, 0, false, 1.0};
        double g[AUDIO_BLOCK_SAMPLES]; double worst = 0;
        // play a while (gain stays 1)
        for (int b=0;b<4;b++){ double s=pauseBlock(c,g); if(s>worst)worst=s; }
        check("gain==1 while playing", std::fabs(c.pauseGain-1.0)<1e-9);
        // pause: ramp down to ~0 within ~16 blocks
        c.paused = true;
        for (int b=0;b<24;b++){ double s=pauseBlock(c,g); if(s>worst)worst=s; }
        check("gain ramps to ~0 while paused", c.pauseGain < 1e-6);
        // unpause: ramp back to ~1
        c.paused = false;
        for (int b=0;b<24;b++){ double s=pauseBlock(c,g); if(s>worst)worst=s; }
        check("gain ramps back to ~1 on resume", std::fabs(c.pauseGain-1.0)<1e-6);
        // click threshold: a 1/16-per-block ramp over 64 samples = ~0.001/sample.
        check("ramp click-free (max per-sample step < 0.005)", worst < 0.005);
    }

    // (5) rapid mute/unmute: position invariant, gain stays in [0,1].
    {
        Clip ref{M/2, 0, 0, false, 1.0};
        Clip tst{M/2, 0, 0, false, 1.0};
        double g[AUDIO_BLOCK_SAMPLES];
        u32 mp = 0; bool boundsOk = true, posOk = true;
        for (u32 k=0;k<2*M;k++){
            tst.paused = (k & 1);              // toggle every block (chord-stab speed)
            mp++; advance(ref, mp, M); advance(tst, mp, M);
            pauseBlock(tst, g);
            if (tst.pauseGain < -1e-9 || tst.pauseGain > 1.0+1e-9) boundsOk = false;
            if (ref.play != tst.play) posOk = false;
        }
        check("rapid toggle: position invariant", posOk);
        check("rapid toggle: gain stays in [0,1]", boundsOk);
    }

    if (g_fails){printf("\n%d FAILURE(S)\n",g_fails);return 1;}
    printf("\nALL PASS\n");return 0;
}
