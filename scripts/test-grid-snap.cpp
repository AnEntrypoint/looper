// Standalone host test: record-start beat-grid snap math (pure logic, no Pi).
// Mirrors loopMachine.cpp latch gate + loopClip.cpp _startRecording phase snap.
// Build: g++ -O2 -std=c++17 scripts/test-grid-snap.cpp -o scripts/test-grid-snap.exe
#include <cstdint>
#include <cstdio>

typedef uint32_t u32;

static int g_fails = 0;
static void check(const char* name, bool cond) {
    if (cond) printf("ok: %s\n", name);
    else { printf("FAIL: %s\n", name); g_fails++; }
}

// gridStep = M/16 (M encodes 16 beats), clamped: < 16 falls back to M.
static u32 grid_step(u32 M) { return (M >= 16) ? (M / 16) : M; }

// Latch gate: a pending record latches when masterPhase sits on a beat-grid
// multiple. Returns the FIRST phase >= press_phase at which the latch fires.
static u32 latch_phase(u32 M, u32 press_phase) {
    u32 g = grid_step(M);
    if (M == 0 || g == 0) return press_phase; // immediate (no grid)
    u32 rem = press_phase % g;
    return rem == 0 ? press_phase : press_phase + (g - rem); // next grid point
}

// Start-phase snap (from _startRecording): round the backdated start phase to
// the nearest grid multiple — floor, then round up if past the midpoint.
static u32 snap_start_phase(u32 M, u32 start_phase) {
    u32 M0 = M;
    if (M0 == 0) return start_phase; // first loop defines grid, sample-true
    u32 g = grid_step(M0);
    if (g == 0) return start_phase;
    u32 rem = start_phase % g;
    u32 snapped = start_phase - rem;
    if (rem * 2 >= g) snapped += g;
    return snapped;
}

int main() {
    // M = 16 beats * 345 blocks/beat = 5520 (example phrase). gridStep = 345.
    const u32 M = 5520;
    const u32 g = grid_step(M);
    check("gridStep = M/16", g == 345);

    // 1. Press exactly on a beat grid point -> latch fires same block.
    check("latch on-grid: zero wait", latch_phase(M, 690) == 690); // 690 = 2*345

    // 2. Press one block before a grid point -> forward-snap to that point.
    check("latch just-before: forward to next grid", latch_phase(M, 689) == 690);

    // 3. Press one block after a grid point -> latch at NEXT grid (gate), but
    //    start-phase SNAP rounds the backdated start back to the just-passed
    //    grid point (since 1 block is well under the midpoint).
    check("snap just-after: backdate to passed grid", snap_start_phase(M, 691) == 690);

    // 4. Snap just-before midpoint stays on the lower grid point.
    check("snap below midpoint floors", snap_start_phase(M, 690 + 172) == 690);    // 172 < 345/2
    // 5. Snap at/above midpoint rounds up to the next grid point.
    check("snap at midpoint rounds up", snap_start_phase(M, 690 + 173) == 1035);   // 173 >= 172.5

    // 6. Latch never waits more than one beat (gridStep), unlike the old
    //    full-phrase gate which could wait the whole M.
    bool within_one_beat = true;
    for (u32 p = 0; p < M; p++) {
        u32 wait = latch_phase(M, p) - p;
        if (wait >= g) { within_one_beat = false; break; }
    }
    check("latch wait < one beat for every press phase", within_one_beat);

    // 7. Degenerate: M < 16 falls back to phrase boundary (gridStep == M).
    check("small M falls back to gridStep=M", grid_step(8) == 8);
    check("small M latch = next phrase", latch_phase(8, 3) == 8);

    // 8. First loop (M==0): no snap, sample-true start.
    check("M==0 snap is identity", snap_start_phase(0, 12345) == 12345);
    check("M==0 latch immediate", latch_phase(0, 12345) == 12345);

    // 9. All snapped start phases are grid multiples (the core requirement:
    //    consecutive recordings slice on a multiple/division of the quant).
    bool all_multiples = true;
    for (u32 p = 0; p < M; p++)
        if (snap_start_phase(M, p) % g != 0) { all_multiples = false; break; }
    check("every snapped start is a grid multiple (no offbeats)", all_multiples);

    if (g_fails) { printf("%d FAILURE(S)\n", g_fails); return 1; }
    printf("ALL PASS\n");
    return 0;
}
