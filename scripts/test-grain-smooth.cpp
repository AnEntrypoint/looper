// Host test for grain-path smoothness (formant-full = the smooth config).
// "Jumps in and out of different moments" / vocal-doubling = the grainFormant
// resplice snapping the grain source to a new moment of past audio. Fewer
// snaps over a steady tone = smoother. This counts snaps directly by watching
// m_inEpoch make a non-Tin jump.
//
// Build: g++ -std=c++17 -O2 -I scripts/engines -o scripts/test-grain-smooth.exe scripts/test-grain-smooth.cpp && ./scripts/test-grain-smooth.exe
#define private public
#include "grainFormant.h"
#include <vector>
#include <cmath>
#include <cstdio>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int main() {
    const int sr = 48000;
    // Continuous tone with realistic slow period jitter (vibrato-ish + noise)
    // — this is what makes the period estimate wobble and the resplice fire.
    float f0 = 196.0f;
    std::vector<float> in;
    double ph = 0;
    for (int n = 0; n < sr * 4; n++) {
        float t = (float)n / sr;
        float vib = 1.0f + 0.01f * sinf(2.0f*M_PI*5.0f*t);   // 5Hz, ±1% vibrato
        double dph = 2.0*M_PI*f0*vib/sr;
        float s = 0; double hp = ph;
        for (int h = 1; h <= 6; h++) { s += (1.0f/h)*(float)sin(hp); hp += dph; }
        in.push_back(0.4f * s);
        ph += dph;
    }

    GrainFormant g;
    g.setScale(0.5f);            // -12
    g.setInputPeriod(245.0);     // 196Hz @ 48k
    g.setFormantFactor(2.0f);    // formant FULL (the smooth config)

    double prevEpoch = 0; bool first = true; int snaps = 0;
    for (size_t i = 0; i < in.size(); i++) {
        g.write(in[i]);
        double beforeEpoch = g.m_inEpoch;
        g.read();
        // A snap = m_inEpoch changed by clearly more than the +Tin per-emit
        // advance (i.e. the resplice fired).
        double delta = g.m_inEpoch - beforeEpoch;
        if (!first && delta > g.m_Tin * 1.5) snaps++;
        first = false;
    }
    printf("grain resplices over 4s @ -12 formant-full (vibrato tone): %d  (fewer = smoother)\n", snaps);
    return 0;
}
