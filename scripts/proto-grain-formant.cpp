// Prototype: discrete-grain PSOLA octaver with INDEPENDENT formant via grain
// playback speed (the research-backed technique). Goal: prove pitch stays at
// exact -12 while a CONSTANT grain-resample ratio shifts formants, stably and
// click-free — before touching the firmware engine.
//
// pitch  = grain EMISSION spacing (output epoch every T/scale samples)
// formant= grain PLAYBACK SPEED (read grain content at rate `formant`)
//
// For -12: scale=0.5, output epochs every 2T. Each output epoch advances the
// INPUT read epoch by T (so each input period is used ~twice = octave down).
// Grain content is read at `formant` samples/step: formant=1 => formants ride
// with pitch (down an octave, natural -12). formant=2 => formants restored to
// original pitch (preserved). 0.5..2 = the knob.
#include <vector>
#include <cmath>
#include <cstdio>
#include <random>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double measureFund(const std::vector<float>& x, int sr) {
    int n = x.size(), mn = sr/400, mx = sr/30, st = n/3;
    double bv=-1e30; int bl=mn;
    std::vector<double> ac(mx+2,0);
    for (int l=mn;l<=mx;l++){double s=0;for(int i=st;i<n-l;i++)s+=x[i]*x[i+l];ac[l]=s;if(s>bv){bv=s;bl=l;}}
    double y0=ac[bl-1],y1=ac[bl],y2=ac[bl+1],d=(y0-2*y1+y2);
    double dl=d!=0?0.5*(y0-y2)/d:0; return (double)sr/(bl+dl);
}
static double goertz(std::vector<float>&x,int sr,double hz){int n=x.size();double w=2*M_PI*hz/sr,c=2*cos(w),s1=0,s2=0;for(int i=n/3;i<n;i++){double s=x[i]+c*s1-s2;s2=s1;s1=s;}return sqrt(s1*s1+s2*s2-c*s1*s2);}
static int clicks(std::vector<float>&x){int n=x.size();if(n<3)return 0;std::vector<double>d(n,0);double m=0;for(int i=2;i<n;i++){d[i]=fabs((double)x[i]-2*x[i-1]+x[i-2]);m+=d[i];}m/=(n-2);double v=0;for(int i=2;i<n;i++){double e=d[i]-m;v+=e*e;}double sg=sqrt(v/(n-2));int c=0,last=-9999;for(int i=2;i<n;i++)if(d[i]>m+10*sg){if(i-last>200)c++;last=i;}return c;}

int main() {
    const int sr = 48000; int n = sr*4; double f0 = 196.0;  // G3
    double T = sr / f0;            // input period (~245 samples)
    double scale = 0.5;            // -12
    std::mt19937 rng(1); std::normal_distribution<float> jit(0,0.002f);

    // harmonic-rich input with a fixed resonance ~1200Hz (so formant shift is visible)
    std::vector<float> in(n);
    for (int i=0;i<n;i++){double t=(double)i/sr;float v=0;
        for(int h=1;h<=24;h++){double fh=h*f0;double env=1.0/(1.0+pow((fh-1200.0)/600.0,2.0));v+=(float)(env*sin(2*M_PI*fh*t));}
        in[i]=v*0.3f+jit(rng);}

    float formants[] = {0.5f, 0.7f, 1.0f, 1.4f, 2.0f};
    printf("fmt   outFund(want 98)  E600   E2400  ratio(2400/600)  clicks\n");
    for (float fm : formants) {
        std::vector<float> out(n, 0.0f);
        // discrete-grain synthesis
        double inEpoch = 2*T;            // input read epoch (start past warmup)
        double outPos  = 2*T;            // next output epoch position
        double grainLen = 2*T;           // grain = 2 periods, Hann window
        // emit grains: output epoch spacing = T/scale = 2T (sets -12 pitch).
        // each emission advances inEpoch by T (input consumed at half rate).
        while (outPos < n - grainLen) {
            int gl = (int)grainLen;
            for (int k = 0; k < gl; k++) {
                double w = 0.5 - 0.5*cos(2*M_PI*k/(gl-1));   // Hann
                // grain content read at `fm` -> formant shift (resample grain)
                double srcPos = inEpoch + (k - gl/2)*(double)fm;
                int si = (int)srcPos; double fr = srcPos - si;
                if (si < 1 || si >= n-1) continue;
                float s = in[si]*(1-fr) + in[si+1]*fr;        // linear interp
                int oi = (int)(outPos + (k - gl/2));
                if (oi >= 0 && oi < n) out[oi] += (float)(w * s);
            }
            outPos  += T/scale;     // = 2T  -> emission rate sets pitch (-12)
            inEpoch += T;           // input advances 1 period per emission
        }
        double of = measureFund(out, sr);
        double e6=goertz(out,sr,600), e24=goertz(out,sr,2400);
        printf("%.1f      %7.1f         %.3f  %.3f   %.3f          %d\n",
               fm, of, e6, e24, e24/e6, clicks(out));
    }
    printf("\n(want: outFund ~98Hz at ALL fmt = pitch independent of formant;\n ratio rises with fmt = formant shifts up; clicks low = clean)\n");
    return 0;
}
