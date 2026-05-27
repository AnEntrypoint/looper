// formant_sweep.cpp — render -12 at a sweep of formant depths, measure graininess
// (high-freq noise / aperiodicity) and chop (50ms-window RMS jumpiness).
#define private public
#include "engine_solad_snac.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static std::vector<float> RD(const char*p,int&sr){FILE*f=fopen(p,"rb");std::vector<float>x;sr=48000;if(!f)return x;uint8_t h[44];if(fread(h,1,44,f)!=44){fclose(f);return x;}int ch=h[22]|(h[23]<<8);sr=h[24]|(h[25]<<8)|(h[26]<<16)|(h[27]<<24);fseek(f,12,SEEK_SET);char id[5]={0};uint32_t sz=0;while(fread(id,1,4,f)==4&&fread(&sz,4,1,f)==1){if(!memcmp(id,"data",4))break;fseek(f,sz,SEEK_CUR);}int n=sz/2;std::vector<int16_t>r(n);fread(r.data(),2,n,f);fclose(f);for(int i=0;i<n;i+=ch)x.push_back(r[i]/32768.0f);return x;}
static double band(const std::vector<float>&x,int sr,double f,int a,int b){double w=2*M_PI*f/sr,c=cos(w),co=2*c,s1=0,s2=0,s0;int i=0;for(float v:x){if(i>=a&&i<b){s0=v+co*s1-s2;s2=s1;s1=s0;}i++;}return s1*s1+s2*s2-co*s1*s2;}
int main(int ac,char**av){
 const char*p=ac>1?av[1]:"scripts/samples/guitar_E2_low_E.wav"; double f0=ac>2?atof(av[2]):82.4;
 int sr;auto in=RD(p,sr);if(in.empty()){printf("no input\n");return 1;}if(sr<=0)sr=48000;int N=in.size();
 double tg=f0*0.5;
 printf("FORMANT SWEEP -12 of %s (bass %.1fHz):\n",p,tg);
 printf("depth gMixEnd  fund-vs-orig  HF-noise(grain)  chop(RMS-jump)  clicks\n");
 for(double d=-1.0; d<=1.0001; d+=0.25){
  EngineSoladSnac e;e.setPitchScale(0.5f);e.setFormantDepth((float)d);
  std::vector<float>o(N);int C=64;
  for(int i=0;i<N;i+=C){int n=std::min(C,N-i);std::vector<float>b(n);for(int k=0;k<n;k++)b[k]=in[i+k]*0.1f;e.processBlock(b.data(),&o[i],n);}
  double h1=band(o,sr,tg,0,N),h0=band(o,sr,f0*2,0,N);
  // graininess: energy above 4*fundamental (3..8kHz noise floor) relative to fund
  double tot=0,hf=0;for(int i=0;i<N;i++)tot+=o[i]*o[i];
  // crude HF via first-difference energy ratio (grain/noise = high d/dt)
  double de=0;for(int i=1;i<N;i++){float df=o[i]-o[i-1];de+=df*df;}
  double grain=10*log10((de/ (tot+1e-12)));
  // chop: stddev of consecutive 50ms RMS window ratios (smooth=low, choppy=high)
  int W=sr/20;std::vector<double>rms;for(int i=0;i+W<=N;i+=W){double s=0;for(int k=0;k<W;k++)s+=o[i+k]*o[i+k];rms.push_back(sqrt(s/W));}
  double jump=0;int nj=0;for(size_t i=1;i<rms.size();i++)if(rms[i]>1e-4&&rms[i-1]>1e-4){double r=fabs(20*log10(rms[i]/rms[i-1]));jump+=r;nj++;}jump=nj?jump/nj:0;
  int clk=0;for(int i=1;i<N;i++)if(fabs(o[i]-o[i-1])>0.5f)clk++;
  printf("%+0.2f  %.3f    %+.1fdB       %+.1fdB           %.2fdB/win     %d\n",d,e.grainMixNow(),10*log10(h1/(h0+1e-12)),grain,jump,clk);
 }
 return 0;}
