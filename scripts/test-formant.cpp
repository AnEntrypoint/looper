// Formant-stage doubling/transient witness for solad-snac at -12.
//
// Doubling test: feed a SPARSE impulse train (one click every 400ms) on a
// quiet sine bed. A clean engine emits each click ONCE. The hard-jump
// pre-resample buffer replays ~85ms of buffer on wrap => a click can be
// emitted TWICE (original + replayed copy). We count output peaks per input
// click: >1 = doubling.
//
// Transient test: feed a single pluck, measure 10-90% onset rise time of the
// output envelope vs the dry passthrough reference. Longer = smear.
//
//   build: g++ -std=c++17 -O2 -I scripts/engines -o scripts/test-formant.exe scripts/test-formant.cpp
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include "engine_solad_snac.h"
static const double PI = 3.14159265358979323846;

static std::vector<double> env10(const std::vector<float>& x, int sr) {
    double tc = 1.0 / (0.005 * sr);
    std::vector<double> e(x.size(), 0.0); double v = 0;
    for (size_t i = 0; i < x.size(); i++) { v += (std::fabs((double)x[i]) - v) * tc; e[i] = v; }
    return e;
}

// Count distinct output envelope peaks above thr in [s,e).
static int countPeaks(const std::vector<double>& e, int s, int en, double thr, int minGapSamp) {
    int cnt = 0, lastPk = -minGapSamp;
    for (int i = s+1; i < en-1 && i < (int)e.size()-1; i++) {
        if (e[i] > thr && e[i] >= e[i-1] && e[i] > e[i+1] && (i - lastPk) >= minGapSamp) {
            cnt++; lastPk = i;
        }
    }
    return cnt;
}

static double onsetRiseMs(const std::vector<float>& out, int sr) {
    auto e = env10(out, sr);
    double peak = 0; size_t pk = 0;
    for (size_t i = 0; i < e.size(); i++) if (e[i] > peak) { peak = e[i]; pk = i; }
    double lo = 0.1*peak, hi = 0.9*peak;
    long iLo=-1,iHi=-1;
    for (size_t i=0;i<=pk;i++){ if(iLo<0&&e[i]>=lo)iLo=i; if(e[i]>=hi){iHi=i;break;} }
    if(iLo<0||iHi<0||iHi<iLo)return -1;
    return 1000.0*(iHi-iLo)/sr;
}

int main() {
    const int SR = 48000;
    printf("formant doubling/transient witness (-12, scale=0.5)\n");
    printf("%-8s  clicks_in  peaks_out(doubling)   pluck_onset_ms\n", "depth");
    float depths[] = { -1.0f, -0.5f, 0.5f, 1.0f };
    for (float d : depths) {
        // sparse click train: 1 click per 400ms over 5s = ~12 clicks
        int N = SR*5; std::vector<float> in(N,0.0f), out(N);
        int clickEvery = (int)(0.4*SR), nclicks=0;
        for (int i = (int)(0.5*SR); i < N; i += clickEvery) {
            for (int k=0;k<48;k++){ double w=0.5-0.5*cos(2*PI*k/47.0); if(i+k<N)in[i+k]+=(float)(0.8*w*sin(2*PI*2000.0*k/SR)); }
            nclicks++;
        }
        EngineSoladSnac e; e.setPitchScale(0.5f); e.setFormantDepth(d);
        for (int i=0;i<N;i+=64){int n=std::min(64,N-i);e.processBlock(&in[i],&out[i],n);}
        auto eo = env10(out, SR);
        double mx=0; for(size_t i=SR;i<eo.size();i++)mx=std::max(mx,eo[i]);
        int peaks = countPeaks(eo, SR, N, mx*0.35, (int)(0.05*SR));
        // pluck transient
        int M=SR; std::vector<float> pin(M),pout(M);
        for(int i=0;i<M;i++){double t=(double)i/SR;double ev=std::exp(-t*6.0);pin[i]=(float)(0.7*ev*sin(2*PI*110.0*i/SR));}
        EngineSoladSnac e2; e2.setPitchScale(0.5f); e2.setFormantDepth(d);
        for(int i=0;i<M;i+=64){int n=std::min(64,M-i);e2.processBlock(&pin[i],&pout[i],n);}
        printf("%+6.1f    %6d       %6d                %.2f\n", d, nclicks, peaks, onsetRiseMs(pout,SR));
    }
    return 0;
}
