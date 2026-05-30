// Standalone host test: first-loop length == press-to-press interval, both ends
// latency-compensated, computed in ABSOLUTE ring coordinates. Models
// loopClip.cpp::_backdatedRecordLength after the absolute-coordinate fix.
// Build: g++ -O2 -std=c++17 scripts/test-stop-trim.cpp -o scripts/test-stop-trim.exe
#include <cstdint>
#include <cstdio>
typedef uint32_t u32;
static const u32 CROSSFADE = 4;  // CROSSFADE_BLOCKS

static int g_fails = 0;
static void check(const char* n, bool c){ if(c) printf("ok: %s\n",n); else {printf("FAIL: %s\n",n); g_fails++;} }

// Reality of the clip recorder: m_recStartBlock is BACKDATED into the past
// (= startPressBlock). Recording copies one ring block per audio update from
// m_recStartBlock forward, while the live write head also advances one per
// update — so the clip runs a constant `backStart` blocks BEHIND the head.
// At stop-command time (live head = wrNow): record_block = wrNow - recStart - backStart.
//
// The fix: length = backdated stopBlock - m_recStartBlock (absolute ring
// coords), clamped to record_block, floored to CROSSFADE*2.
static u32 backdated_len(u32 startPressBlock, u32 stopPressBlock, u32 wrNow, u32 backStart) {
    u32 recStart = startPressBlock;                     // m_recStartBlock = backdated start press
    u32 record_block = wrNow - recStart - backStart;    // clip catch-up lag
    u32 stopBlock = stopPressBlock;                     // cbBackdatedBlock(stop)
    u32 len = (stopBlock > recStart) ? (stopBlock - recStart) : 0;
    if (len > record_block) len = record_block;
    u32 floorLen = CROSSFADE * 2;
    if (len < floorLen) len = (record_block >= floorLen) ? floorLen : record_block;
    if (len == 0) len = 1;
    return len;
}

int main() {
    // True press-to-press = stopPress - startPress, regardless of how far the
    // live head ran past the stop press (process latency) or how far behind the
    // clip recorder lagged (backStart). startPress=1000, stopPress=1100 => 100.
    check("len == press-to-press (100), head ran to 1110, backStart 6",
          backdated_len(1000, 1100, 1110, 6) == 100);

    // Different process latency (head at 1130) — length must NOT change.
    check("len independent of how late the stop COMMAND processed",
          backdated_len(1000, 1100, 1130, 6) == 100);

    // Different start backdate — length must NOT change (absolute coords).
    check("len independent of backStart magnitude",
          backdated_len(1000, 1100, 1115, 12) == 100);

    // Longer loop.
    check("len 500 press-to-press", backdated_len(2000, 2500, 2520, 8) == 500);

    // Clamp: if the stop press is somehow ahead of what's been copied yet, clamp
    // to record_block (degenerate, shouldn't happen with real backdates).
    // record_block = 1050 - 1000 - 40 = 10; stopBlock-recStart=49 -> clamp to 10.
    check("clamps to captured record_block when stop>captured",
          backdated_len(1000, 1049, 1050, 40) == 10);

    // Floor: tiny interval floors to CROSSFADE*2 (8) if enough was captured.
    // startPress=1000 stopPress=1003 => interval 3; record_block plenty -> 8.
    check("tiny interval floors to CROSSFADE*2",
          backdated_len(1000, 1003, 1100, 4) == CROSSFADE * 2);

    if (g_fails) { printf("%d FAILURE(S)\n", g_fails); return 1; }
    printf("ALL PASS\n");
    return 0;
}
