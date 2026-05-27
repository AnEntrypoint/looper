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
static double goer(const std::vector<float>&x,int sr,double f){double w=2*M_PI*f/sr,c=cos(w),co=2*c,s0=0,s1=0,s2=0;for(float v:x){s0=v+co*s1-s2;s2=s1;s1=s0;}return s1*s1+s2*s2-co*s1*s2;}
int main(){struct S{const char*n;const char*p;double f0;}S[]={{"E2",   "scripts/samples/guitar_E2_low_E.wav",82.4},{"A2","scripts/samples/guitar_A2_A_str.wav",110},{"D3","scripts/samples/guitar_D3_D_str.wav",146.8},{"G3","scripts/samples/guitar_G3_G_str.wav",196}};
 printf("FIXED-gate -12 at LOW gain (0.1 = Pi-like quiet input):\n");
 printf("%-3s lock%%  period(true)  H1-vs-H2  latency\n","snd");
 for(auto&s:S){int sr;auto in=RD(s.p,sr);if(in.empty())continue;if(sr<=0)sr=48000;
  EngineSoladSnac e;e.setPitchScale(0.5f);std::vector<float>o(in.size());int N=in.size(),C=64,l=0,t=0;
  for(int i=0;i<N;i+=C){int n=std::min(C,N-i);std::vector<float>b(n);for(int k=0;k<n;k++)b[k]=in[i+k]*0.1f;e.processBlock(b.data(),&o[i],n);t++;if(e.periodOk())l++;}
  double tg=s.f0*0.5;
  printf("%-3s %3.0f%%   %4d(%4d)    %+.1fdB    %dsamp/%.1fms\n",s.n,100.0*l/t,e.periodNow(),(int)(sr/s.f0),10*log10(goer(o,sr,tg)/(goer(o,sr,tg*2)+1e-9)),e.getInitialReadOffset(),e.getInitialReadOffset()*1000.0/sr);}
 return 0;}
