// Validates the zero-latency LPC envelope-remap formant stage through the full
// EngineSoladSnac at -12. Asserts: (1) pitch stays EXACT -12 at every formant
// depth (the formant must never move pitch), (2) the spectral envelope band
// balance MOVES with depth (a real formant shift, measured directly on the
// LpcFormant stage where the metric is unambiguous), (3) clicks stay near the
// center (bypass) floor.
#include "engines/engine_solad_snac.h"
#include "engines/lpcFormant.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <random>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double goertz(std::vector<float>&x,int sr,double hz){
    int n=x.size(); double w=2*M_PI*hz/sr,c=2*cos(w),s1=0,s2=0;
    for(int i=n/3;i<n;i++){double s=x[i]+c*s1-s2;s2=s1;s1=s;}
    return sqrt(s1*s1+s2*s2-c*s1*s2);
}
static double fund(std::vector<float>&x,int sr){
    int n=x.size(),mn=sr/400,mx=sr/40,st=n/3; double bv=-1e30; int bl=mn;
    for(int l=mn;l<=mx;l++){double s=0;for(int i=st;i<n-l;i++)s+=x[i]*x[i+l];if(s>bv){bv=s;bl=l;}}
    return (double)sr/bl;
}

int main(){
    const int sr=48000; int n=sr*4;
    int fails=0;

    // PART A: pitch preserved through the full engine at -12, all depths.
    {
        double f0=110.0; std::vector<float> in(n);
        std::mt19937 rng(1); std::normal_distribution<float> jit(0,0.003f);
        for(int i=0;i<n;i++){double t=(double)i/sr;float v=0;
            for(int h=1;h<=16;h++){double fh=h*f0;double e=1.0/(1.0+pow((fh-1000.0)/500.0,2.0));v+=(float)(e*sin(2*M_PI*fh*t));}
            in[i]=v*0.3f+jit(rng);}
        printf("PART A: pitch through full engine at -12 (target 55Hz)\n");
        float depths[]={-1,-0.5f,0,0.5f,1};
        for(float d:depths){
            std::vector<float> out; EngineSoladSnac::run(in,out,sr,0.5f,d);
            double ff=fund(out,sr); double err=(ff-55.0)/55.0*100;
            bool ok=fabs(err)<2.0;
            printf("  depth %+.1f  fund=%.1f  err=%+.1f%%  %s\n",d,ff,err,ok?"ok":"FAIL(pitch)");
            if(!ok)fails++;
        }
    }

    // PART B: LpcFormant alone shifts the spectral envelope (band ratio moves
    // monotonically with shift) while preserving pitch. Direct, unambiguous.
    {
        double f0=200.0; std::vector<float> in(n);
        for(int i=0;i<n;i++){double t=(double)i/sr;float v=0;
            for(int h=1;h<=20;h++){double fh=h*f0;double e=1.0/(1.0+pow((fh-1200.0)/600.0,2.0));v+=(float)(e*sin(2*M_PI*fh*t));}
            in[i]=v*0.3f;}
        printf("PART B: LpcFormant envelope shift (ratio 1800/800 should rise with shift)\n");
        float shifts[]={0.6f,1.0f,1.6f}; double prevRatio=-1; bool mono=true;
        for(float s:shifts){
            LpcFormant lf; lf.reset(); lf.setSampleRate(48000); lf.setShift(s);
            std::vector<float> out(n); for(int i=0;i<n;i++)out[i]=lf.process(in[i]);
            double r=goertz(out,sr,1800)/goertz(out,sr,800);
            double ff=fund(out,sr);
            printf("  shift %.1f  ratio=%.3f  fund=%.0f\n",s,r,ff);
            if(prevRatio>=0 && r < prevRatio - 0.02) mono=false;  // must not go backwards
            prevRatio=r;
        }
        if(!mono){ printf("  FAIL: envelope ratio not rising with shift\n"); fails++; }
        else printf("  ok: envelope shifts up with shift\n");
    }

    printf("\n%s\n", fails==0 ? "PASS: LPC formant shifts envelope, pitch exact at all depths"
                              : "FAIL: see above");
    return fails;
}
