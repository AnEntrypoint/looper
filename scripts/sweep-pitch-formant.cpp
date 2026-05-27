// Local sweep: -12 and +12, each at formant min/med/max, over several sounds.
// Emits per-combo metrics AND writes output WAVs so the user can audition every
// candidate. No Pi needed — pure host run of the real EngineSoladSnac.
//
// Build:
//   g++ -std=c++17 -O2 -I scripts/engines -o scripts/sweep-pitch-formant.exe scripts/sweep-pitch-formant.cpp
// Run (writes WAVs + a results table to scripts/sweep-out/):
//   ./scripts/sweep-pitch-formant.exe
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

// ---- minimal WAV I/O (16-bit mono/stereo -> mono float) ----
static std::vector<float> readWav(const char* path, int& sr) {
    FILE* f = fopen(path, "rb"); std::vector<float> x; sr = 48000;
    if (!f) return x;
    uint8_t hdr[44]; if (fread(hdr,1,44,f)!=44) { fclose(f); return x; }
    int ch = hdr[22] | (hdr[23]<<8);
    sr = hdr[24]|(hdr[25]<<8)|(hdr[26]<<16)|(hdr[27]<<24);
    int bits = hdr[34]|(hdr[35]<<8);
    // find data chunk (handle non-44 headers loosely)
    fseek(f, 12, SEEK_SET); char id[5]={0}; uint32_t sz=0;
    while (fread(id,1,4,f)==4 && fread(&sz,4,1,f)==1) {
        if (memcmp(id,"data",4)==0) break;
        fseek(f, sz, SEEK_CUR);
    }
    if (bits!=16) { fclose(f); return x; }
    int n = sz/2;
    std::vector<int16_t> raw(n); fread(raw.data(),2,n,f); fclose(f);
    for (int i=0;i<n;i+=ch) x.push_back(raw[i]/32768.0f);  // left/mono
    return x;
}
static void writeWav(const std::string& path, const std::vector<float>& x, int sr) {
    FILE* f = fopen(path.c_str(),"wb"); if(!f) return;
    uint32_t n=x.size(), bytes=n*2, riff=36+bytes;
    auto w32=[&](uint32_t v){fwrite(&v,4,1,f);}; auto w16=[&](uint16_t v){fwrite(&v,2,1,f);};
    fwrite("RIFF",1,4,f); w32(riff); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); w32(16); w16(1); w16(1); w32(sr); w32(sr*2); w16(2); w16(16);
    fwrite("data",1,4,f); w32(bytes);
    for (float s: x){ int v=(int)(s*32767.0f); if(v>32767)v=32767; if(v<-32768)v=-32768; int16_t o=(int16_t)v; fwrite(&o,2,1,f);}
    fclose(f);
}

// ---- metrics ----
static int countClicks(const std::vector<float>& y){
    if(y.size()<3)return 0; double m=0,s=0; std::vector<float>d(y.size());
    for(size_t i=1;i<y.size();i++){d[i]=fabsf(y[i]-y[i-1]);m+=d[i];}
    m/=y.size(); for(size_t i=1;i<y.size();i++)s+=(d[i]-m)*(d[i]-m); s=sqrt(s/y.size());
    int c=0; for(size_t i=1;i<y.size();i++) if(d[i]>m+6*s)c++; return c;
}
// onset count (transient doubling detector)
static int onsets(const std::vector<float>& y,int sr){
    float es=0,ef=0,efp=0;int cool=0,o=0;
    for(float s:y){float a=fabsf(s);es+=(a-es)*0.0008f;ef+=(a-ef)*0.02f;float dd=ef-efp;efp=ef;
      if(cool>0){cool--;continue;} if(ef>es*3.0f&&ef>0.02f&&dd>0.002f){o++;cool=(int)(0.12f*sr);}}
    return o;
}
// dominant freq via parabolic-interpolated autocorrelation (rough)
static float domFreq(const std::vector<float>& y,int sr){
    int N=std::min((int)y.size(),1<<15); if(N<256)return 0;
    int minLag=sr/1000, maxLag=sr/40; double best=-1; int bl=minLag;
    for(int lag=minLag;lag<maxLag;lag++){double a=0;for(int i=0;i<N-lag;i++)a+=y[i]*y[i+lag];
      if(a>best){best=a;bl=lag;}}
    return bl>0? (float)sr/bl : 0;
}
static double rms(const std::vector<float>& y){double s=0;for(float v:y)s+=v*v;return y.size()?sqrt(s/y.size()):0;}

int main(){
    struct Sound{ const char* name; const char* path; float inHz; };
    Sound sounds[] = {
        {"E2_lowE",   "scripts/samples/guitar_E2_low_E.wav", 82.4f},
        {"A2",        "scripts/samples/guitar_A2_A_str.wav",110.0f},
        {"C3_alesis", "scripts/samples/guitar_C3_48k.wav",  130.8f},
        {"G3",        "scripts/samples/guitar_G3_G_str.wav",196.0f},
        {"pluck_E2",  "scripts/quality-corpus/pluck_E2.wav", 82.4f},
        {"transients","scripts/quality-corpus/transients.wav", 0.0f},
    };
    struct Pitch{ const char* name; float scale; };
    Pitch pitches[] = { {"-12",0.5f}, {"+12",2.0f} };
    struct Form{ const char* name; float depth; };
    Form forms[] = { {"min",-1.0f}, {"med",0.0f}, {"max",1.0f} };

    system("mkdir scripts\\sweep-out 2>nul");
    FILE* rep = fopen("scripts/sweep-out/results.csv","w");
    fprintf(rep,"sound,pitch,formant,in_hz,out_hz,out_target_hz,fund_err_hz,rms_out,clicks,onsets_in,onsets_out,doubling\n");
    printf("%-11s %-4s %-4s | out_hz (tgt) err  rms    clicks onsets(in>out) double\n","sound","pit","fmt");

    for (auto& snd : sounds) {
        int sr; std::vector<float> in = readWav(snd.path, sr);
        if (in.empty()) { printf("  [skip %s — not found]\n", snd.name); continue; }
        if (sr<=0) sr=48000;
        int onIn = onsets(in,sr);
        for (auto& p : pitches) {
            float tgt = snd.inHz * p.scale;
            for (auto& fm : forms) {
                std::vector<float> out;
                EngineSoladSnac::run(in, out, sr, p.scale, fm.depth);
                float oHz = domFreq(out,sr);
                float err = (snd.inHz>0)? oHz - tgt : 0;
                int cl = countClicks(out); int onO = onsets(out,sr);
                bool dbl = onO > onIn + 1;
                char fn[256];
                snprintf(fn,sizeof fn,"scripts/sweep-out/%s_%s_%s.wav", snd.name, p.name, fm.name);
                writeWav(fn, out, sr);
                printf("%-11s %-4s %-4s | %6.1f (%5.1f) %+5.1f %.3f %5d  %2d>%-2d   %s\n",
                       snd.name,p.name,fm.name, oHz,tgt,err, rms(out), cl, onIn,onO, dbl?"DBL":"-");
                fprintf(rep,"%s,%s,%s,%.1f,%.1f,%.1f,%.1f,%.4f,%d,%d,%d,%d\n",
                        snd.name,p.name,fm.name,snd.inHz,oHz,tgt,err,rms(out),cl,onIn,onO,dbl?1:0);
            }
        }
    }
    fclose(rep);
    printf("\nWAVs + results.csv written to scripts/sweep-out/\n");
    return 0;
}
