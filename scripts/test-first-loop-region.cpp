// Standalone host test: the FIRST loop plays back its EXACT recorded region,
// seamlessly, every cycle — and the record->play handoff (TAIL -> finish ->
// play) does NOT flip to LOOPING mid-tail nor jump the play head. Models the
// loopClipUpdate.cpp advance FSM + _startEndingRecording/_finishRecording/
// _startPlaying play-head rules after the rationalization.
// Build: g++ -O2 -std=c++17 scripts/test-first-loop-region.cpp -o scripts/test-first-loop-region.exe
#include <cstdint>
#include <cstdio>
#include <vector>
typedef uint32_t u32;

static int g_fails = 0;
static void check(const char* n, bool c){ if(c) printf("ok: %s\n",n); else {printf("FAIL: %s\n",n); g_fails++;} }

// Mirror of the relevant clip state machine for the FIRST loop (no master yet).
enum St { RECORDING, RECORDING_MAIN, RECORDING_TAIL, RECORDED, PLAYING, LOOPING };
static const u32 CROSSFADE = 4;

struct Clip {
    St st = RECORDING;
    u32 play = 0, rec = 0, num = 0, max = 0;
    u32 xfstart = 0, xfoff = 0;
    int running = 0;          // incDecRunning accounting (must never double count)
    u32 masterLen = 0;        // 0 until first loop defines it
};

// loopClipUpdate pp_main advance (rationalized): TAIL never drives state.
static void advance_play(Clip& c) {
    if (c.st == RECORDING_TAIL) { c.play++; if (c.play >= c.num) c.play = 0; return; }
    if (c.masterLen > 0 && c.num < c.masterLen) { /* sub-phrase: not first loop */ }
    else {
        c.play++;
        if (c.play == c.num && c.st == PLAYING) {     // _startCrossFade, PLAYING only
            c.st = LOOPING; c.xfstart = c.num; c.play = 0; c.xfoff = 0; c.running++;  // main head wraps to 0
        } else if (c.play >= c.num) c.play = 0;
    }
}

static void finish_recording(Clip& c) {            // record_block reached max
    bool willPlay = (c.st == RECORDING_TAIL);
    c.st = RECORDED; c.running--;
    if (willPlay) {                                // _startPlaying(preserve=true)
        if (c.num > 0 && c.play >= c.num) c.play %= c.num;
        c.xfstart = 0; c.xfoff = 0; c.st = PLAYING; c.running++;
    }
}

int main() {
    Clip c;
    const u32 region = 20;   // recorded loop length (blocks), > CROSSFADE
    // Record `region` + CROSSFADE blocks of input into the clip (block i == i).
    std::vector<u32> clipbuf;

    // --- record phase: fill clip from ring (block content = absolute index) ---
    for (u32 i = 0; i < region; i++) {
        clipbuf.push_back(i);                       // exact recorded region
        c.rec++;
        if (c.st == RECORDING && c.rec >= CROSSFADE) c.st = RECORDING_MAIN;
    }
    // stop pressed: trim to region, enter TAIL, record the crossfade tail.
    c.num = region; c.max = region + CROSSFADE; c.masterLen = region; // first loop defines master
    c.st = RECORDING_TAIL; c.play = 0; c.running++;  // incDecNumRecordedClips/_startEndingRecording
    // TAIL records tail blocks region..max while playing back for monitoring.
    for (u32 i = region; i < c.max; i++) {
        clipbuf.push_back(i);                        // tail (seam overlap content)
        advance_play(c);                             // monitor advance (no state flip!)
        c.rec++;
        if (c.rec >= c.max) { finish_recording(c); break; }
    }

    check("TAIL did not flip to LOOPING mid-tail", c.st == PLAYING);
    check("running count is balanced (no double-count)", c.running == 1);
    check("num_blocks == recorded region (exact)", c.num == region);

    // --- play several cycles, collect the played block indices ---
    std::vector<u32> played;
    for (int i = 0; i < (int)region * 3 + 5; i++) {
        played.push_back(c.play);                    // block read THIS update
        advance_play(c);
        if (c.st == LOOPING) {                        // settle the crossfade instantly for the model
            c.st = PLAYING; c.xfstart = 0; c.xfoff = 0; c.running--;
        }
    }

    // Every played block must be within the recorded region [0, region).
    bool inRegion = true;
    for (u32 b : played) if (b >= region) { inRegion = false; break; }
    check("every played block is within the recorded region", inRegion);

    // The play head must wrap 0..region-1..0 with no gap/dupe at the seam: find
    // the first 0 after the start, the run before it must be a contiguous ramp.
    bool clean_wrap = true;
    for (size_t i = 1; i < played.size(); i++) {
        u32 prev = played[i-1], cur = played[i];
        bool ok = (cur == prev + 1) || (cur == 0 && prev == region - 1);
        if (!ok) { clean_wrap = false; printf("  seam break at %zu: %u->%u\n", i, prev, cur); break; }
    }
    check("play head wraps region-1 -> 0 with no shift/gap/dupe", clean_wrap);

    // Determinism: the loop repeats the SAME sequence every cycle.
    bool repeats = true;
    for (size_t i = 0; i + region < played.size(); i++)
        if (played[i] != played[i + region]) { repeats = false; break; }
    check("playback repeats the exact region every cycle", repeats);

    if (g_fails) { printf("%d FAILURE(S)\n", g_fails); return 1; }
    printf("ALL PASS\n");
    return 0;
}
