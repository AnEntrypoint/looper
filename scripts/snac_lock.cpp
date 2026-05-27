#define private public
#include "engine_solad_snac.h"
#include <vector>
#include <cstdio>
#include <cstdint>
#include <cstring>
static std::vector<float> rd(const char*p,int&sr){FILE*f=fopen(p,"rb");std::vector<float>x;sr=48000;if(!f)return x;uint8_t h[44];if(fread(h,1,44,f)!=44){fclose(f);return x;}int ch=h[22]|(h[23]<<8);sr=h[24]|(h[25]<<8)|(h[26]<<16)|(h[27]<<24);fseek(f,12,SEEK_SET);char id[5]={0};uint32_t sz=0;while(fread(id,1,4,f)==4&&fread(&sz,4,1,f)==1){if(!memcmp(id,"data",4))break;fseek(f,sz,SEEK_CUR);}int n=sz/2;std::vector<int16_t>r(n);fread(r.data(),2,n,f);fclose(f);for(int i=0;i<n;i+=ch)x.push_back(r[i]/32768.0f);return x;}
int main(){int sr;auto in=rd("scripts/samples/guitar_E2_low_E.wav",sr);if(sr<=0)sr=48000;
 EngineSoladSnac e;e.setPitchScale(0.5f);std::vector<float>out(in.size());int N=in.size(),C=64;
 int lockedBlocks=0,tot=0;for(int i=0;i<N;i+=C){int n=std::min(C,N-i);e.processBlock(&in[i],&out[i],n);tot++;if(e.periodOk())lockedBlocks++;}
 printf("E2 host: SNAC locked %d/%d blocks (%.0f%%), final period=%d (true E2=%d)\n",lockedBlocks,tot,100.0*lockedBlocks/tot,e.periodNow(),(int)(sr/82.4));
 return 0;}
