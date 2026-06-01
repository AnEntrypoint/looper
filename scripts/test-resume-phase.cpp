// Pause/resume phrase-sync: a resumed clip must play the SAME grid position a
// never-paused clip would, for BOTH playback regimes (sub-phrase L<M phase-locked,
// and first-loop / phrase-or-longer L>=M self-advancing).
//
// Mirrors loopClipUpdate.cpp playback advance + loopClip.cpp _startPlaying.
// Reference clip: plays continuously. Test clip: pauses (CS_RECORDED) then resumes
// (_startPlaying re-anchors play_block) at an arbitrary later masterPhase. The two
// must agree on play_block at every subsequent masterPhase.
//
// Build: g++ -O2 -std=c++17 scripts/test-resume-phase.cpp -o scripts/test-resume-phase.exe
#include <cstdio>
#include <cstdint>
typedef uint32_t u32;
static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

struct Clip {
    u32 L;        // num_blocks
    u32 offset;   // recordStartPhaseOffset
    u32 play;     // play_block
    bool playing; // CS_PLAYING vs CS_RECORDED(paused)
};

// _startPlaying: re-anchor play_block from masterPhase (the in-tree formula).
static void startPlaying(Clip& c, u32 masterPhase, u32 M) {
    if (M > 0 && c.L > 0)
        c.play = ((masterPhase - c.offset) % c.L + c.L) % c.L;
    else
        c.play = 0;
    c.playing = true;
}

// One playback-advance block (loopClipUpdate). M = masterLoopBlocks.
static void advance(Clip& c, u32 masterPhase, u32 M) {
    if (!c.playing) return;
    if (M > 0 && c.L <= M) {
        // FIX: sub-phrase AND first-loop (L<=M) both phase-locked to the grid via
        // one formula. L==M is coherent: (masterPhase-offset)%M advances by 1 each
        // block and wraps at M exactly like self-advance, but re-derivable from
        // masterPhase so pause/resume lands on the same grid position.
        c.play = ((masterPhase - c.offset) % c.L + c.L) % c.L;
    } else {
        // phrase-or-longer (L>M) / no-master: self-advance
        c.play++;
        if (c.play >= c.L) c.play = 0;
    }
}

// Simulate: reference plays from masterPhase=startPhase; test pauses at pausePhase
// and resumes at resumePhase. Compare play_block from resumePhase onward.
static bool sim(u32 M, u32 L, u32 offset, u32 startPhase, u32 pausePhase, u32 resumePhase, const char* label) {
    Clip ref{L, offset, 0, false};
    Clip tst{L, offset, 0, false};
    startPlaying(ref, startPhase, M);
    startPlaying(tst, startPhase, M);

    u32 mp = startPhase;
    // advance both to pausePhase
    for (; mp < pausePhase; mp++) { advance(ref, mp+1, M); advance(tst, mp+1, M); }
    // test pauses
    tst.playing = false;
    // both continue masterPhase; ref keeps playing, test is silent
    for (; mp < resumePhase; mp++) { advance(ref, mp+1, M); /* tst paused */ }
    // test resumes at resumePhase
    startPlaying(tst, resumePhase, M);
    // advance both and compare
    bool agree = true;
    u32 firstDiff = 0, refAt = 0, tstAt = 0;
    for (u32 k = 0; k < 4*M + 7; k++) {
        if (ref.play != tst.play) { agree = false; firstDiff = mp; refAt = ref.play; tstAt = tst.play; break; }
        mp++; advance(ref, mp, M); advance(tst, mp, M);
    }
    printf("  [%s] M=%u L=%u off=%u resume@%u: %s", label, M, L, offset, resumePhase,
           agree ? "in-sync\n" : "");
    if (!agree) printf("OFFBEAT (mp=%u ref.play=%u tst.play=%u)\n", firstDiff, refAt, tstAt);
    return agree;
}

int main(){
    const u32 M = 5520;       // phrase
    // Sub-phrase clip (L=M/4), offset on a beat grid.
    check("sub-phrase L=M/4 resume mid-phrase stays grid-synced",
          sim(M, M/4, 0, 0, M/8, M/4 + 37, "subphrase"));
    // First loop L==M, offset 0 (grid-defining clip).
    check("first-loop L==M off=0 resume stays grid-synced",
          sim(M, M, 0, 0, M/3, M + 91, "firstloop-off0"));
    // First loop L==M but offset NON-zero (recorded mid-phrase, then masterPhase
    // reset modulo M leaves offset%M != 0) — the suspected offbeat case.
    check("first-loop L==M off=1234 resume stays grid-synced",
          sim(M, M, 1234, 0, M/3, M + 91, "firstloop-offNZ"));
    // Phrase-or-longer L=2M.
    check("L=2M resume stays grid-synced",
          sim(M, 2*M, 0, 0, M/2, M + 200, "L2M"));
    // ---- First-loop exact-region + seam invariant under the phase-lock fix ----
    // A first loop (L==M) playing continuously from its downbeat must: cover every
    // block 0..L-1 in order, return to block 0 exactly at the phrase wrap, and the
    // wrap (next==0 && play>0) must fire exactly once per L cycle.
    {
        u32 L = M, off = 1234;
        Clip c{L, off, 0, false};
        // At the recorded downbeat, masterPhase == off => play_block 0.
        u32 mp = off;
        startPlaying(c, mp, M);
        bool startsAtZero = (c.play == 0);
        u32 seamCount = 0;
        bool coveredInOrder = true;
        u32 expect = 0;
        for (u32 k = 0; k < L; k++) {
            if (c.play != expect) coveredInOrder = false;
            u32 prev = c.play;
            mp++; advance(c, mp, M);
            // seam = wrap from a positive block back to 0
            if (c.play == 0 && prev > 0) seamCount++;
            expect = (expect + 1) % L;
        }
        check("first loop starts at block 0 at its recorded downbeat", startsAtZero);
        check("first loop covers [0,L) in order", coveredInOrder);
        check("first loop seam (wrap to 0) fires exactly once per cycle", seamCount == 1);
    }

    if (g_fails){printf("%d FAILURE(S)\n",g_fails);return 1;}
    printf("ALL PASS\n");return 0;
}
