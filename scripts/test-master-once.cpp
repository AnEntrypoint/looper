// Standalone host test: the master phrase reference is set EXACTLY ONCE by the
// first loop on a clear bank, and NEVER grown/resliced by later loops. Mirrors
// the loopClip.cpp::_startEndingRecording master-set guard after the fix.
// Build: g++ -O2 -std=c++17 scripts/test-master-once.cpp -o scripts/test-master-once.exe
#include <cstdint>
#include <cstdio>
#include <initializer_list>
typedef uint32_t u32;

static int g_fails = 0;
static void check(const char* n, bool c){ if(c) printf("ok: %s\n",n); else {printf("FAIL: %s\n",n); g_fails++;} }

struct Machine { u32 masterLoopBlocks; u32 masterPhase; };

// The fixed guard: set the master only when none exists yet (== 0). Returns
// true if this call defined the master.
static bool finish_loop(Machine& m, bool linkSynced, u32 num_blocks, u32 phaseAtFinish) {
    m.masterPhase = phaseAtFinish; // playing clips advanced the global phase
    if (!linkSynced && m.masterLoopBlocks == 0) {
        m.masterLoopBlocks = num_blocks;
        m.masterPhase = m.masterPhase % num_blocks;
        return true;
    }
    return false;
}

int main() {
    Machine m{0, 0};

    // First loop on a clear bank defines the master.
    bool d1 = finish_loop(m, false, 5520, 5520);
    check("first loop defines master", d1 && m.masterLoopBlocks == 5520);
    u32 masterAfter1 = m.masterLoopBlocks;

    // Second loop, SHORTER — must not touch the master.
    u32 phaseBefore = (m.masterPhase = 1234);
    bool d2 = finish_loop(m, false, 2760, 1234);
    check("2nd (shorter) loop does NOT redefine master", !d2 && m.masterLoopBlocks == masterAfter1);
    check("2nd loop does NOT reslice masterPhase", m.masterPhase == phaseBefore);

    // Third loop, LONGER than the master — the exact case that used to grow the
    // master and corrupt existing loops. Must be a no-op on the grid.
    m.masterPhase = 4000;
    bool d3 = finish_loop(m, false, 11040, 4000); // 2x the master length
    check("3rd (longer) loop does NOT grow master", !d3 && m.masterLoopBlocks == 5520);
    check("3rd loop does NOT reslice masterPhase", m.masterPhase == 4000);

    // Many subsequent loops of varying length never change the master.
    u32 keep = m.masterLoopBlocks;
    bool anyRedefined = false;
    for (u32 nb : {1000u, 9000u, 5520u, 22080u, 700u}) {
        if (finish_loop(m, false, nb, 999)) anyRedefined = true;
        if (m.masterLoopBlocks != keep) anyRedefined = true;
    }
    check("no later loop ever redefines the master", !anyRedefined);

    // Link-synced: first loop does NOT define the local master (peer owns grid).
    Machine ms{0, 0};
    bool dsync = finish_loop(ms, true, 5520, 5520);
    check("synced: first loop defers master to peer", !dsync && ms.masterLoopBlocks == 0);

    if (g_fails) { printf("%d FAILURE(S)\n", g_fails); return 1; }
    printf("ALL PASS\n");
    return 0;
}
