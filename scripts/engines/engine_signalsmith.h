// Wrapper around signalsmith-stretch for the quality harness.
// Uses the UPSTREAM signalsmith-stretch (pulled into /tmp/sigsmith-upstream)
// for host testing — the patches/signalsmith stub ships its own STL replacements
// for the Pi bare-metal build which conflict with the host's libstdc++.
#pragma once
#include <vector>
#include "signalsmith-stretch.h"   // -I /tmp/sigsmith-upstream/signalsmith-stretch

inline void engine_signalsmith(const std::vector<float> &in,
                               std::vector<float> &out,
                               int sr, float scale,
                               int blockSamples, int intervalSamples,
                               bool splitComp = false)
{
    signalsmith::stretch::SignalsmithStretch<float> s;
    s.configure(1, blockSamples, intervalSamples, splitComp);
    s.setTransposeFactor(scale, 0.0f);
    int n = (int)in.size();
    out.assign(n, 0.0f);
    const float *inA[1] = { in.data() };
    float       *outA[1] = { out.data() };
    s.process(inA, n, outA, n);
}
