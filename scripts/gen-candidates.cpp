// Generate many -12 candidate configs locally (clean continuous-reader path,
// formant=med). Sweeps respliceFrac (smoothness vs lag) x xfadeScale (splice
// crossfade length) and emits one WAV per candidate + a metrics table, so the
// user can audition and pick. No Pi needed.
//
// Build: g++ -std=c++17 -O2 -I scripts/engines -o scripts/gen-candidates.exe scripts/gen-candidates.cpp
// Run:   ./scripts/gen-candidates.exe
#define private public
#include "engine_solad_snac.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static std::vector<float> readWav(const char* path,int& sr){
    FILE* f=fopen(path,"rb"); std::vector<float> x; sr=48000; if(!f)return x;
    uint8_t h[44]; if(fread(h,1,44,f)!=44){fclose(f);return x;}
    int ch=h[22]|(h[23]<<8); sr=h[24]|(h[25]<<8)|(h[26]<<16)|(h[27]<<24); int bits=h[34]|(h[35]<<8);
    fseek(f,12,SEEK_SET); char id[5]={0}; uint32_t sz=0;
    while(fread(id,1,4,f)==4 && fread(&sz,4,1,f)==1){ if(!memcmp(id,"data",4))break; fseek(f,sz,SEEK_CUR);}
    if(bits!=16){fclose(f);return x;} int n=sz/2; std::vector<int16_t>r(n); fread(r.data(),2,n,f); fclose(f);
    for(int i=0;i<n;i+=ch)x.push_back(r[i]/32768.0f); return x;
}
static void writeWav(const std::string& p,const std::vector<float>& x,int sr){
    FILE* f=fopen(p.c_str(),"wb"); if(!f)return; uint32_t n=x.size(),b=n*2,riff=36+b;
    auto w32=[&](uint32_t v){fwrite(&v,4,1,f);}; auto w16=[&](uint16_t v){fwrite(&v,2,1,f);};
    fwrite("RIFF",1,4,f);w32(riff);fwrite("WAVE",1,4,f);fwrite("fmt ",1,4,f);w32(16);w16(1);w16(1);w32(sr);w32(sr*2);w16(2);w16(16);
    fwrite("data",1,4,f);w32(b);
    for(float s:x){int v=(int)(s*32767.0f);if(v>32767)v=32767;if(v<-32768)v=-32768;int16_t o=(int16_t)v;fwrite(&o,2,1,f);}
    fclose(f);
}
static int countClicks(const std::vector<float>& y){
    if(y.size()<3)return 0; double m=0,s=0; std::vector<float>d(y.size());
    for(size_t i=1;i<y.size();i++){d[i]=fabsf(y[i]-y[i-1]);m+=d[i];} m/=y.size();
    for(size_t i=1;i<y.size();i++)s+=(d[i]-m)*(d[i]-m); s=sqrt(s/y.size());
    int c=0; for(size_t i=1;i<y.size();i++) if(d[i]>m+6*s)c++; return c;
}

// run engine with explicit tunables, return output + splice count
static std::vector<float> runTuned(const std::vector<float>& in,int sr,float scale,
                                   float formant,float respliceFrac,float xfade,
                                   unsigned& splices){
    EngineSoladSnac e;
    e.setPitchScale(scale);
    e.setFormantDepth(formant);
    e.setRespliceFrac(respliceFrac);
    e.setXfadeScale(xfade);
    std::vector<float> out(in.size(),0.0f);
    int N=in.size(),C=64;
    for(int i=0;i<N;i+=C){int n=std::min(C,N-i); e.processBlock(&in[i],&out[i],n);}
    splices = e.m_spliceCount;
    return out;
}

int main(){
    struct S{const char* name;const char* path;};
    S sounds[]={{"E2","scripts/samples/guitar_E2_low_E.wav"},
                {"G3","scripts/samples/guitar_G3_G_str.wav"}};
    float fracs[] = {8.0f, 16.0f, 32.0f, 64.0f};      // smoothness vs lag
    float xfades[]= {1.0f, 2.0f, 3.0f};                // splice crossfade len
    system("mkdir scripts\\candidates 2>nul");
    FILE* rep=fopen("scripts/candidates/results.csv","w");
    fprintf(rep,"sound,respliceFrac,xfade,splices,clicks\n");
    printf("Candidates @ -12 formant=med (clean path). Lower splices+clicks = smoother.\n");
    printf("%-3s frac xfade | splices clicks\n","snd");
    for(auto& snd:sounds){
        int sr; std::vector<float> in=readWav(snd.path,sr); if(in.empty()){printf("[skip %s]\n",snd.name);continue;} if(sr<=0)sr=48000;
        for(float fr:fracs)for(float xf:xfades){
            unsigned sp=0;
            std::vector<float> out=runTuned(in,sr,0.5f,0.0f,fr,xf,sp);
            int cl=countClicks(out);
            char fn[256]; snprintf(fn,sizeof fn,"scripts/candidates/%s_frac%g_xf%g.wav",snd.name,fr,xf);
            writeWav(fn,out,sr);
            printf("%-3s %4g %5g | %6u %6d\n",snd.name,fr,xf,sp,cl);
            fprintf(rep,"%s,%g,%g,%u,%d\n",snd.name,fr,xf,sp,cl);
        }
    }
    fclose(rep);
    printf("\nWAVs + results.csv in scripts/candidates/\n");
    return 0;
}
