// GhostXForm + measurement aggregation tests (linkGhost.h), tied to the wire
// codec's sample math (linkWire.h lwMeasurementSamples). Proves: the host<->ghost
// map round-trips; the median is correct (odd/even); and a simulated PING/PONG
// burst with a known true clock offset + symmetric jitter recovers that offset.
//
// Build: g++ -O2 -std=c++17 scripts/test-link-ghost.cpp -o scripts/test-link-ghost.exe
#include <cstdio>
#include <cstdint>
#include "../linkWire.h"
#include "../linkGhost.h"

static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

int main()
{
    // ---- ghost transform round-trip ----
    {
        LinkGhostXForm x; x.offsetMicros = 1234567;
        int64_t host = 9000000;
        int64_t ghost = lgHostToGhost(x, host);
        check("hostToGhost adds offset", ghost == host + 1234567);
        check("ghostToHost inverts hostToGhost", lgGhostToHost(x, ghost) == host);
        // negative offset (our clock ahead of the session)
        x.offsetMicros = -500;
        check("round-trip with negative offset", lgGhostToHost(x, lgHostToGhost(x, 42)) == 42);
    }

    // ---- median correctness ----
    {
        LinkMeasurement m; lgMeasReset(&m);
        int64_t odd[] = {5, 1, 3, 2, 4};                 // median 3
        lgMeasAddSamples(&m, odd, 5);
        check("median of {1,2,3,4,5} == 3", lgMeasMedian(&m) == 3);
        lgMeasReset(&m);
        int64_t even[] = {10, 2, 8, 4};                  // sorted 2,4,8,10 -> (4+8)/2=6
        lgMeasAddSamples(&m, even, 4);
        check("median of {2,4,8,10} == 6", lgMeasMedian(&m) == 6);
        lgMeasReset(&m);
        check("median of empty == 0", lgMeasMedian(&m) == 0);
    }

    // ---- end-to-end: recover a known offset from simulated exchanges ----
    // Model: initiator pings at host send time s; pong received at s+rtt; with
    // symmetric latency the responder stamped ghost at the midpoint (s + rtt/2)
    // mapped to ghost = midpoint + TRUE_OFFSET. So per exchange:
    //   host(send)=s, prevHost(=recv)=s+rtt, ghost = (s + rtt/2) + TRUE_OFFSET
    // lwMeasurementSamples then yields s1 = ghost - (send+recv)/2 = TRUE_OFFSET.
    {
        const int64_t TRUE_OFFSET = 1000000;   // 1 s
        LinkMeasurement m; lgMeasReset(&m);
        int64_t s = 0;
        // deterministic symmetric jitter in [-20,+20] us via a small LCG
        uint32_t rng = 12345;
        for (int i = 0; i < 60; i++) {
            rng = rng * 1103515245u + 12345u;
            int64_t jit = (int64_t)((rng >> 16) % 41) - 20;     // [-20,20]
            int64_t rtt = 400 + ((rng >> 8) % 200);             // 400..599 us
            int64_t send = s;
            int64_t recv = s + rtt;
            int64_t ghost = (send + rtt/2) + TRUE_OFFSET + jit;  // symmetric -> ~TRUE_OFFSET
            int64_t out[2];
            int k = lwMeasurementSamples(send, recv, ghost, /*prevGhost*/0, out);
            lgMeasAddSamples(&m, out, k);
            s += LG_PING_INTERVAL_US;
        }
        int64_t med = lgMeasMedian(&m);
        int64_t err = med > TRUE_OFFSET ? med - TRUE_OFFSET : TRUE_OFFSET - med;
        check("measured median recovers true offset within jitter (<=20us)", err <= 20);
        LinkGhostXForm x = lgMeasResult(&m);
        check("lgMeasResult offset == median", x.offsetMicros == med);
        // The recovered transform maps a host time to the session clock to within jitter.
        int64_t h = 5000000, g = lgHostToGhost(x, h);
        check("recovered xform maps host->ghost ~ host+TRUE_OFFSET",
              (g - (h + TRUE_OFFSET)) <= 20 && ((h + TRUE_OFFSET) - g) <= 20);
    }

    // ---- done / exhausted predicates ----
    {
        LinkMeasurement m; lgMeasReset(&m);
        check("not done when empty", !lgMeasHasEnough(&m));
        int64_t one = 7;
        for (int i = 0; i < LG_NUM_DATAPOINTS; i++) lgMeasAddSamples(&m, &one, 1);
        check("done at LG_NUM_DATAPOINTS samples", lgMeasHasEnough(&m));
        check("median of all-equal == that value", lgMeasMedian(&m) == 7);
        m.pings = LG_NUM_MEASUREMENTS;
        check("exhausted at LG_NUM_MEASUREMENTS pings", lgMeasExhausted(&m));
    }

    // ---- accumulator capacity clamp (no overflow) ----
    {
        LinkMeasurement m; lgMeasReset(&m);
        int64_t buf[8] = {1,1,1,1,1,1,1,1};
        for (int i = 0; i < LG_MAX_SAMPLES; i++) lgMeasAddSamples(&m, buf, 8);  // way over
        check("sample count clamps at LG_MAX_SAMPLES", m.n == LG_MAX_SAMPLES);
    }

    printf(g_fails ? "\n%d FAIL\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
