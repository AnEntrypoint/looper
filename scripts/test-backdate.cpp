// Standalone host test: latency-backdate timing math (pure logic, no Pi/hardware).
// Build: g++ -O2 -std=c++17 scripts/test-backdate.cpp -o scripts/test-backdate.exe
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static const uint32_t SAMPLE_RATE = 44100;
static const uint64_t FIXED_LAG_SAMPLES = 96;
static const uint64_t BLOCK = 64;

static int g_fails = 0;

static void check(const char* name, bool cond) {
    if (cond) printf("ok: %s\n", name);
    else { printf("FAIL: %s\n", name); g_fails++; }
}

// Wrap-safe elapsed microseconds between a press timestamp and process time.
static uint32_t elapsed_us(uint32_t press_ticks, uint32_t now_ticks) {
    return (uint32_t)(now_ticks - press_ticks); // unsigned wrap is well-defined
}

// Core backdate computation. press_ticks==0 is the "no timestamp" sentinel.
// ring_blocks_available bounds the history cap. *clamped reports whether the cap fired.
static uint64_t compute_backdate(uint32_t press_ticks, uint32_t now_ticks,
                                 uint64_t ring_blocks_available, bool* clamped) {
    uint64_t backdate;
    if (press_ticks == 0) {
        backdate = FIXED_LAG_SAMPLES; // sentinel: skip elapsed term
    } else {
        uint64_t us = elapsed_us(press_ticks, now_ticks);
        uint64_t elapsed_samples = us * (uint64_t)SAMPLE_RATE / 1000000ULL;
        backdate = elapsed_samples + FIXED_LAG_SAMPLES;
    }
    uint64_t max_backdate = ring_blocks_available * BLOCK;
    bool did_clamp = false;
    if (backdate > max_backdate) { backdate = max_backdate; did_clamp = true; }
    if (clamped) *clamped = did_clamp;
    return backdate;
}

int main() {
    // 1. Normal case: 3000us @ 44100 = 132 samples, +96 = 228.
    {
        bool cl = false;
        uint64_t expected_samples = (uint64_t)3000 * SAMPLE_RATE / 1000000ULL; // 132
        uint64_t expect = expected_samples + FIXED_LAG_SAMPLES;                // 228
        uint64_t got = compute_backdate(1000, 4000, /*blocks*/ 10000, &cl);
        check("normal: 3000us -> 228 samples", got == expect && expect == 228 && !cl);
    }

    // 2. Wrap case: 0xFFFFF000 -> 0x00001000 = 0x2000 = 8192us.
    {
        bool cl = false;
        uint32_t us = elapsed_us(0xFFFFF000u, 0x00001000u);
        uint64_t expected_samples = (uint64_t)8192 * SAMPLE_RATE / 1000000ULL; // 361
        uint64_t expect = expected_samples + FIXED_LAG_SAMPLES;
        uint64_t got = compute_backdate(0xFFFFF000u, 0x00001000u, 10000, &cl);
        bool sane = got > FIXED_LAG_SAMPLES && got < 10000; // positive, not a huge wrong value
        check("wrap: diff is 8192us", us == 0x2000u);
        check("wrap: backdate correct and sane", got == expect && sane && !cl);
    }

    // 3. Sentinel: press_ticks==0 -> only fixed lag.
    {
        bool cl = false;
        uint64_t got = compute_backdate(0, 4000, 10000, &cl);
        check("sentinel: backdate == FIXED_LAG_SAMPLES", got == FIXED_LAG_SAMPLES && !cl);
    }

    // 4. Clamp: 500ms elapsed, tiny ring history -> clamps to MAX_BACKDATE.
    {
        bool cl = false;
        uint32_t now = 1000 + 500000; // 500ms after press at t=1000
        uint64_t ring_blocks = 100;   // MAX_BACKDATE = 6400 samples
        uint64_t max_backdate = ring_blocks * BLOCK;
        uint64_t got = compute_backdate(1000, now, ring_blocks, &cl);
        check("clamp: flag set", cl);
        check("clamp: clamped to MAX_BACKDATE", got == max_backdate && got == 6400);
    }

    // 5. Length invariance: start AND stop both backdated by the same latency.
    // Recorded length in blocks must equal the press interval in blocks,
    // independent of the (shared) latency value.
    {
        auto press_to_block = [](uint32_t press_ticks, uint32_t process_ticks,
                                 uint64_t process_block) -> int64_t {
            // Backdate the process position by the press latency, in blocks.
            uint64_t bd = compute_backdate(press_ticks, process_ticks, 100000, nullptr);
            return (int64_t)process_block - (int64_t)(bd / BLOCK);
        };

        // True musical interval between the two button presses.
        uint32_t t0 = 100000;  // start press time (us)
        uint32_t t1 = 1100000; // stop press time (us): exactly 1,000,000us later
        uint64_t interval_blocks = ((uint64_t)(t1 - t0) * SAMPLE_RATE / 1000000ULL) / BLOCK;

        bool all_ok = true;
        // Try several different (but identical-for-both-presses) processing latencies.
        for (uint32_t lat : {2000u, 50000u, 250000u}) {
            uint32_t proc0 = t0 + lat;
            uint32_t proc1 = t1 + lat;
            // Each press processed `lat` us after it occurred; both backdated the same way.
            uint64_t proc0_block = ((uint64_t)proc0 * SAMPLE_RATE / 1000000ULL) / BLOCK;
            uint64_t proc1_block = ((uint64_t)proc1 * SAMPLE_RATE / 1000000ULL) / BLOCK;
            int64_t startBlock = press_to_block(t0, proc0, proc0_block);
            int64_t stopBlock  = press_to_block(t1, proc1, proc1_block);
            int64_t length = stopBlock - startBlock;
            // Allow +/-1 block for integer-division rounding of fixed-lag subtraction.
            if (!(length >= (int64_t)interval_blocks - 1 && length <= (int64_t)interval_blocks + 1)) {
                all_ok = false;
                printf("  (lat=%u) length=%lld interval=%llu\n",
                       lat, (long long)length, (unsigned long long)interval_blocks);
            }
        }
        check("length invariance: recorded length == press interval, latency-independent", all_ok);
    }

    if (g_fails) {
        printf("%d FAILURE(S)\n", g_fails);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
