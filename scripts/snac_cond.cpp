#define private public
#include "engine_solad_snac.h"
#include <vector>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
static std::vector<float> rd(const char*p,int&sr){FILE*f=fopen(p,"rb");std::vector<float>x;sr=48000;if(!f)return x;uint8_t h[44];if(fread(h,1,44,f)!=44){fclose(f);return x;}int ch=h[22]|(h[23]<<8);sr=h[24]|(h[25]<<8)|(h[26]<<16)|(h[27]<<24);fseek(f,12,SEEK_SET);char id[5]={0};uint32_t sz=0;while(fread(id,1,4,f)==4&&fread(&sz,4,1,f)==1){if(!memcmp(id,"data",4))break;fseek(f,sz,SEEK_CUR);}int n=sz/2;std::vector<int16_t>r(n);fread(r.data(),2,n,f);fclose(f);for(int i=0;i<n;i+=ch)x.push_back(r[i]/32768.0f);return x;}
static double lockpct(std::vector<float>in,float gain){EngineSoladSnac e;e.setPitchScale(0.5f);std::vector<float>out(in.size());int N=in.size(),C=64,lk=0,tot=0;for(int i=0;i<N;i+=C){int n=std::min(C,N-i);std::vector<float>blk(n);for(int k=0;k<n;k++)blk[k]=in[i+k]*gain;e.processBlock(blk.data(),&out[i],n);tot++;if(e.periodOk())lk++;}return 100.0*lk/tot;}
// crude resample to 44100
static std::vector<float> resamp(const std::vector<float>&x,double r){std::vector<float>o;for(double p=0;p<x.size()-1;p+=r){int i=(int)p;double f=p-i;o.push_back(x[i]*(1-f)+x[i+1]*f);}return o;}
int main(){int sr;auto in=rd("scripts/samples/guitar_E2_low_E.wav",sr);if(sr<=0)sr=48000;
 printf("SNAC lock%% vs input gain (Pi input may be quieter):\n");
 for(float g:{1.0f,0.5f,0.25f,0.1f,0.05f})printf("  gain %.2f: %.0f%%\n",g,lockpct(in,g));
 // at 44100 internal rate (resample input 48k->44.1k = factor 48/44.1=1.0884)
 auto in441=resamp(in,48000.0/44100.0);printf("resampled to 44100-equiv: %.0f%% lock\n",lockpct(in441,1.0f));
 return 0;}
