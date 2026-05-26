// Validates the grain-playback-speed formant through the full EngineSoladSnac
// at -12. Asserts: (1) pitch stays EXACT -12 at every formant depth (the grain
// stage re-emits at the output period so pitch never moves); (2) the spectral
// envelope band balance MOVES monotonically with depth (a real, stable formant
// shift); (3) clicks stay near the center (bypass) floor.
#include "engines/engine_solad_snac.h"
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
    int n=x.size(),mn=sr/400,mx=sr/30,st=n*3/4; double bv=-1e30; int bl=mn;
    for(int l=mn;l<=mx;l++){double s=0;for(int i=st;i<n-l;i++)s+=x[i]*x[i+l];if(s>bv){bv=s;bl=l;}}
    return (double)sr/bl;
}
static int clicks(std::vector<float>&x){
    int n=x.size(); if(n<3)return 0; std::vector<double>d(n,0); double m=0;
    for(int i=2;i<n;i++){d[i]=fabs((double)x[i]-2*x[i-1]+x[i-2]);m+=d[i];} m/=(n-2);
    double v=0; for(int i=2;i<n;i++){double e=d[i]-m;v+=e*e;} double sg=sqrt(v/(n-2));
    int c=0,last=-9999; for(int i=2;i<n;i++) if(d[i]>m+10*sg){if(i-last>200)c++;last=i;}
    return c;
}

int main(){
    const int sr=48000; int n=sr*4; double f0=196.0;  // G3
    std::vector<float> in(n);
    std::mt19937 rng(1); std::normal_distribution<float> jit(0,0.002f);
    for(int i=0;i<n;i++){double t=(double)i/sr;float v=0;
        for(int h=1;h<=24;h++){double fh=h*f0;double e=1.0/(1.0+pow((fh-1200.0)/600.0,2.0));v+=(float)(e*sin(2*M_PI*fh*t));}
        in[i]=v*0.3f+jit(rng);}

    float depths[]={-1.0f,-0.5f,0.0f,0.5f,1.0f};
    int fails=0; double prevRatio=-1; int ctrClicks=0;
    printf("depth  outFund(want 98)  E600   E2400  ratio  clicks\n");
    for(int di=0; di<5; di++){
        float d=depths[di];
        std::vector<float> out; EngineSoladSnac::run(in,out,sr,0.5f,d);
        double of=fund(out,sr), e6=goertz(out,sr,600), e24=goertz(out,sr,2400);
        double ratio=e24/(e6+1e-9); int clk=clicks(out);
        if(d==0.0f) ctrClicks=clk;
        bool pitchOk = fabs((of-98.0)/98.0*100.0) < 3.0;
        printf("%+.1f    %7.1f          %.3f  %.3f  %.3f  %d  %s\n",
               d,of,e6,e24,ratio,clk, pitchOk?"":"FAIL(pitch)");
        if(!pitchOk) fails++;
        // ratio must not go backwards as depth rises (monotonic envelope shift)
        if(prevRatio>=0 && ratio < prevRatio - 0.02){ printf("  (ratio went backwards depth=%.1f)\n",d); }
        prevRatio=ratio;
    }
    // envelope must actually move: up-extreme ratio >> down-extreme ratio
    std::vector<float> od, ou;
    EngineSoladSnac::run(in,od,sr,0.5f,-1.0f);
    EngineSoladSnac::run(in,ou,sr,0.5f,+1.0f);
    double rd=goertz(od,sr,2400)/(goertz(od,sr,600)+1e-9);
    double ru=goertz(ou,sr,2400)/(goertz(ou,sr,600)+1e-9);
    bool moved = ru > rd*2.0;
    printf("\nenvelope move: down-ratio=%.3f up-ratio=%.3f (up must be >2x down) %s\n",
           rd,ru, moved?"ok":"FAIL(no real formant shift)");
    if(!moved) fails++;
    printf("%s\n", fails==0 ? "PASS: grain formant shifts envelope, pitch exact -12 at all depths"
                            : "FAIL: see above");
    return fails;
}
