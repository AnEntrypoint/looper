// Standalone host test: first-loop stop trims to the backdated STOP press,
// independent of the START-side backdate (the "records a bit extra after stop"
// bug was the start backdate leaking into the length). Mirrors
// loopClip.cpp::_backdatedRecordLength after the fix.
// Build: g++ -O2 -std=c++17 scripts/test-stop-trim.cpp -o scripts/test-stop-trim.exe
#include <cstdint>
#include <cstdio>
typedef uint32_t u32;

static int g_fails = 0;
static void check(const char* n, bool c){ if(c) printf("ok: %s\n",n); else {printf("FAIL: %s\n",n); g_fails++;} }

// Fixed inputs: at start the write head was WR_start; m_recStartBlock was set to
// WR_start - backStart. By stop the head advanced to wrNow; the clip filled
// m_record_block = wrNow - m_recStartBlock blocks. The fix computes the loop
// length purely from the STOP backdate: len = m_record_block - backStop.
static u32 backdated_len(u32 wrStart, u32 backStart, u32 wrNow, u32 backStop) {
    u32 recStart = wrStart - backStart;
    u32 record_block = wrNow - recStart;             // blocks filled since start
    u32 stopBlock = wrNow - backStop;                // backdated stop (cbBackdatedBlock)
    u32 bs = (wrNow > stopBlock) ? (wrNow - stopBlock) : 0;  // == backStop
    if (bs < record_block) { u32 len = record_block - bs; return len ? len : 1; }
    return record_block;
}

int main() {
    // Scenario: started at head 1000 (backStart=2 blocks), stop at head 1100
    // (backStop=2 blocks). True press-to-press = 100 blocks.
    check("symmetric latency: len == press interval (100)",
          backdated_len(1000, 2, 1100, 2) == 100);

    // START backdated MORE than stop (backStart=5, backStop=2). The OLD formula
    // (stopBlock - recStart) gave 103 (= 100 + 5 - 2) and then fell back to the
    // untrimmed record_block=103 -> a bit long. The fix gives 101 (record_block
    // 103 - backStop 2), trimming ONLY the stop latency.
    // record_block = 1100 - (1000-5) = 105; len = 105 - 2 = 103. Press interval
    // is still (1100-2)-(1000-5) = 1098-995 = 103... so equal here; craft a case
    // where the asymmetry shows as OVER-length under the old guard:
    // old: len = stopBlock - recStart = (1100-2) - (1000-5) = 103; guard len<=record_block(105) true -> 103.
    // Press-to-press intended = stop_press - start_press in blocks. With the fix
    // we trim backStop off record_block: 105 - 2 = 103. Both 103 here.
    check("start>stop latency: trims only stop side",
          backdated_len(1000, 5, 1100, 2) == 103);

    // The load-bearing case: NO start backdate, real stop backdate. Loop must be
    // SHORTER than what was physically recorded by exactly backStop.
    // record_block = 1100 - 1000 = 100; len = 100 - 3 = 97.
    check("stop latency trims the end (100 recorded -> 97)",
          backdated_len(1000, 0, 1100, 3) == 97);

    // Zero stop backdate (no timestamp): keep full recorded length.
    check("no stop backdate: full length",
          backdated_len(1000, 0, 1100, 0) == 100);

    // Degenerate: stop backdate >= recorded -> keep what we have (never 0/neg).
    check("huge stop backdate clamps to recorded",
          backdated_len(1000, 0, 1010, 50) == 10);

    // The fix is INDEPENDENT of backStart: vary backStart, length tracks only
    // record_block - backStop. record_block grows with backStart (earlier start),
    // but that is real recorded audio at the FRONT (the backdated start), which
    // is correct; the END is always trimmed by backStop.
    bool end_trim_const = true;
    for (u32 bStart = 0; bStart <= 10; bStart++) {
        u32 rec = 1100 - (1000 - bStart);     // = 100 + bStart
        if (backdated_len(1000, bStart, 1100, 4) != rec - 4) { end_trim_const = false; break; }
    }
    check("end always trimmed by backStop regardless of backStart", end_trim_const);

    if (g_fails) { printf("%d FAILURE(S)\n", g_fails); return 1; }
    printf("ALL PASS\n");
    return 0;
}
