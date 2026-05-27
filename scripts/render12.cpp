// render12.cpp — render the -12 output of the DEPLOYED engine on a real guitar
// sample, write it as a WAV, and analyze the bytes: fundamental tracking,
// harmonic structure, envelope (alive vs static), clicks, DC/clipping.
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
static void WR(const char*p,const std::vector<float>&x,int sr){FILE*f=fopen(p,"wb");if(!f)return;uint32_t n=x.size();uint32_t ds=n*2,sz=36+ds;uint16_t ch=1,bps=16,ba=2;uint32_t br=sr*2;fwrite("RIFF",1,4,f);fwrite(&sz,4,1,f);fwrite("WAVEfmt ",1,8,f);uint32_t f16=16;uint16_t pcm=1;fwrite(&f16,4,1,f);fwrite(&pcm,2,1,f);fwrite(&ch,2,1,f);fwrite(&sr,4,1,f);fwrite(&br,4,1,f);fwrite(&ba,2,1,f);fwrite(&bps,2,1,f);fwrite("data",1,4,f);fwrite(&ds,4,1,f);for(float v:x){int s=(int)lround(v*32767);if(s>32767)s=32767;if(s<-32768)s=-32768;int16_t o=s;fwrite(&o,2,1,f);}fclose(f);}
static double goer(const std::vector<float>&x,int sr,double f,int a,int b){double w=2*M_PI*f/sr,c=cos(w),co=2*c,s0=0,s1=0,s2=0;int i=0;for(float v:x){if(i>=a&&i<b){s0=v+co*s1-s2;s2=s1;s1=s0;}i++;}return s1*s1+s2*s2-co*s1*s2;}
int main(int ac,char**av){
 const char*p=ac>1?av[1]:"scripts/samples/guitar_E2_low_E.wav"; double f0=ac>2?atof(av[2]):82.4;
 int sr;auto in=RD(p,sr);if(in.empty()){printf("no input %s\n",p);return 1;}if(sr<=0)sr=48000;
 EngineSoladSnac e;e.setPitchScale(0.5f);int N=in.size();std::vector<float>o(N);int C=64;
 for(int i=0;i<N;i+=C){int n=std::min(C,N-i);std::vector<float>b(n);for(int k=0;k<n;k++)b[k]=in[i+k]*0.1f;e.processBlock(b.data(),&o[i],n);}
 char out[256];snprintf(out,sizeof out,"scripts/render12_out.wav");WR(out,o,sr);
 // BYTE-LEVEL ANALYSIS of the rendered output:
 double tg=f0*0.5; // target -12 fundamental
 // 1) harmonic ladder at the bass fundamental
 double h1=goer(o,sr,tg,0,N),h2=goer(o,sr,tg*2,0,N),h3=goer(o,sr,tg*3,0,N),h0=goer(o,sr,f0*2,0,N);
 // 2) envelope "alive": RMS in 50ms windows, report dynamic range (static tone ~0)
 int W=sr/20;double rmsMin=1e9,rmsMax=0;int nw=0;for(int i=0;i+W<=N;i+=W){double s=0;for(int k=0;k<W;k++)s+=o[i+k]*o[i+k];double r=sqrt(s/W);if(r>1e-4){if(r<rmsMin)rmsMin=r;if(r>rmsMax)rmsMax=r;nw++;}}
 // 3) clicks: count samples whose abs jump from prev > 0.5 (gross discontinuity)
 int clicks=0;for(int i=1;i<N;i++)if(fabs(o[i]-o[i-1])>0.5f)clicks++;
 // 4) DC offset + peak (clipping)
 double dc=0,pk=0;for(float v:o){dc+=v;if(fabs(v)>pk)pk=fabs(v);}dc/=N;
 printf("RENDER -12 of %s (true %.1fHz -> bass %.1fHz)\n",p,f0,tg);
 printf("  wrote %s (%d samp, %.2fs @%dHz)\n",out,N,(double)N/sr,sr);
 printf("  harmonic ladder (dB rel H1): H1=0.0  H2=%+.1f  H3=%+.1f   orig-octave(%.0fHz)=%+.1f\n",
   10*log10(h2/h1),10*log10(h3/h1),f0*2,10*log10(h0/h1));
 printf("  fundamental vs orig octave: %+.1f dB (positive = bass dominates = strong fund)\n",10*log10(h1/(h0+1e-12)));
 printf("  envelope alive: RMS %.4f..%.4f over %d windows, dynamic range %.1f dB (>3dB = alive, not static)\n",
   rmsMin,rmsMax,nw,20*log10(rmsMax/(rmsMin+1e-12)));
 printf("  clicks(|jump|>0.5): %d   DC offset: %.5f   peak: %.3f %s\n",clicks,dc,pk,pk>0.99?"(CLIPPING!)":"(headroom ok)");
 return 0;}
