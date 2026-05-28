// Standalone host test: derive Ableton-Link tempo + quant from a recorded loop length.
// PURE-LOGIC. No Pi, no project headers.
//   g++ -O2 -std=c++17 scripts/test-link-quant.cpp -o scripts/test-link-quant.exe
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// ---- algorithm under test -------------------------------------------------
// Choose musical beat-count so tempo lands closest to 120 BPM, kept in [80,160].
static const double kCandidates[] = {0.25, 0.5, 1, 2, 4, 8, 16};
static const double kTargetBpm = 120.0;
static const double kBpmLo = 80.0;
static const double kBpmHi = 160.0;

void deriveQuant(double clip_seconds, double &out_beats, double &out_bpm, double &out_beat_unit) {
    double bestAny = -1, bestAnyDist = 1e18;     // closest-to-120 ignoring window
    double bestWin = -1, bestWinDist = 1e18;     // closest-to-120 within window
    for (double B : kCandidates) {
        double bpm = 60.0 * B / clip_seconds;
        double dist = std::fabs(bpm - kTargetBpm);
        if (dist < bestAnyDist) { bestAnyDist = dist; bestAny = B; }
        if (bpm >= kBpmLo && bpm <= kBpmHi && dist < bestWinDist) {
            bestWinDist = dist; bestWin = B;
        }
    }
    // Prefer the overall closest-to-120 if it is in window; else the in-window nearest.
    double chosen;
    double bestAnyBpm = 60.0 * bestAny / clip_seconds;
    if (bestAnyBpm >= kBpmLo && bestAnyBpm <= kBpmHi) chosen = bestAny;
    else if (bestWin >= 0)                            chosen = bestWin;
    else                                              chosen = bestAny; // nothing in window: fall back

    out_beats = chosen;
    out_bpm = 60.0 * chosen / clip_seconds;
    out_beat_unit = chosen; // single-emission loop: quant subdivision == the loop's beat span
}

std::string quantLabel(double beats) {
    if (beats < 1.0) {
        // fractional beat -> "1/4 beat", "1/2 beat"
        int denom = (int)std::lround(1.0 / beats);
        return "1/" + std::to_string(denom) + " beat";
    }
    if (beats == 1.0) return "1 beat";
    if (std::fmod(beats, 4.0) == 0.0) {
        int bars = (int)std::lround(beats / 4.0);
        return (bars == 1 ? "1 bar" : std::to_string(bars) + " bars");
    }
    // whole-beat but not a bar multiple
    long b = std::lround(beats);
    return std::to_string(b) + " beats";
}

// ---- test harness ---------------------------------------------------------
static int g_fail = 0;

static void check(bool cond, const std::string &msg) {
    printf("  %s %s\n", cond ? "ok  " : "FAIL", msg.c_str());
    if (!cond) g_fail++;
}

static bool approx(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

struct Case {
    const char *name;
    double clip;
    double expBeats;
    double expBpm;     // <0 => "near", use looser check
    bool   exactBpm;
    const char *expLabel; // nullptr => skip label check
};

int main() {
    std::vector<Case> cases = {
        {"1. 2.0s = 1 bar @120",       2.0,   4,    120.0, true,  "1 bar"},
        {"2. 0.5s = 1 beat @120",      0.5,   1,    120.0, true,  "1 beat"},
        {"3. 0.125s = 1/4 beat @120",  0.125, 0.25, 120.0, false, "1/4 beat"},
        {"4. 4.0s = 2 bars @120",      4.0,   8,    120.0, true,  "2 bars"},
        {"5. 1.8s off-tempo -> 4b",    1.8,   4,    133.333, false,"1 bar"},
        {"6. 2.4s -> 4b @100",         2.4,   4,    100.0, true,  "1 bar"},
    };

    for (const auto &c : cases) {
        double beats, bpm, unit;
        deriveQuant(c.clip, beats, bpm, unit);
        std::string label = quantLabel(beats);
        printf("Case %s: clip=%.3fs -> beats=%g bpm=%.3f quant=\"%s\"\n",
               c.name, c.clip, beats, bpm, label.c_str());

        check(beats == c.expBeats,
              "beats == " + std::to_string(c.expBeats));

        if (c.exactBpm)
            check(approx(bpm, c.expBpm, 1e-9),
                  "bpm exact == " + std::to_string(c.expBpm));
        else
            check(approx(bpm, c.expBpm, 0.5),
                  "bpm near " + std::to_string(c.expBpm));

        if (c.expLabel)
            check(label == c.expLabel,
                  std::string("label == \"") + c.expLabel + "\"");

        // unit == beats for single-emission loop
        check(unit == beats, "beat_unit == beats");

        // Case 7 sanity: bpm in window for every case.
        check(bpm >= kBpmLo && bpm <= kBpmHi,
              "bpm within [80,160]");
        printf("\n");
    }

    // Explicit case-5 anti-assertion: must NOT pick 66.7bpm (2 beats).
    {
        double beats, bpm, unit;
        deriveQuant(1.8, beats, bpm, unit);
        check(!approx(bpm, 66.667, 1.0), "1.8s did NOT pick 66.7bpm (2 beats)");
    }

    if (g_fail) {
        printf("\n%d CHECK(S) FAILED\n", g_fail);
        return 1;
    }
    printf("\nALL PASS\n");
    return 0;
}
