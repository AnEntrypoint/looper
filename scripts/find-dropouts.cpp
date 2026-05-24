// Detect amplitude dropouts: runs of low-amplitude samples in audio that
// should be at signal level. Prints timestamps + durations.
// Use to spot USB-IN underruns / dispatch stalls / silence holes.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

struct Wav { int sr; std::vector<float> s; };
static Wav readWav(const std::string& p) {
    Wav w{}; FILE* f = fopen(p.c_str(), "rb"); if (!f) return w;
    char id[5]; id[4]=0;
    fread(id,4,1,f); uint32_t fsz; fread(&fsz,4,1,f); fread(id,4,1,f);
    fread(id,4,1,f); uint32_t fmtSz; fread(&fmtSz,4,1,f);
    uint16_t fmt; fread(&fmt,2,1,f); uint16_t ch; fread(&ch,2,1,f);
    uint32_t sr; fread(&sr,4,1,f); uint32_t br; fread(&br,4,1,f);
    uint16_t ba; fread(&ba,2,1,f); uint16_t bps; fread(&bps,2,1,f);
    if (fmtSz > 16) fseek(f, fmtSz-16, SEEK_CUR);
    while (fread(id,4,1,f)==1) {
        uint32_t sz; fread(&sz,4,1,f);
        if (std::string(id,4)=="data") {
            int n = sz / ba;
            w.s.resize(n);
            for (int i = 0; i < n; i++) {
                int16_t l, r;
                fread(&l,2,1,f);
                if (ch==2) fread(&r,2,1,f);
                w.s[i] = (float)l / 32768.0f;
            }
            break;
        } else { fseek(f, sz, SEEK_CUR); }
    }
    w.sr = sr; fclose(f);
    return w;
}
int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s wav [silence_db=-40]\n", argv[0]); return 1; }
    Wav w = readWav(argv[1]);
    int N = (int)w.s.size();
    if (N == 0) { fprintf(stderr, "empty wav\n"); return 2; }

    // Envelope: full-wave-rectify + 5ms LPF
    std::vector<float> env(N);
    float e = 0;
    float a = 1.0f - std::exp(-1.0f / (w.sr * 0.005f));
    for (int i = 0; i < N; i++) {
        float r = std::fabs(w.s[i]);
        e += (r - e) * a;
        env[i] = e;
    }
    // Rolling average envelope (200ms) = "expected level"
    int win = w.sr / 5;
    double sum = 0;
    for (int i = 0; i < win && i < N; i++) sum += env[i];
    std::vector<float> avg(N, 0);
    for (int i = win; i < N; i++) {
        avg[i] = (float)(sum / win);
        sum += env[i] - env[i - win];
    }
    // Drop = env < avg * 0.2 (>14 dB below) for >= 1ms
    int minDropSamples = w.sr / 1000;
    int inDrop = 0, dropStart = 0;
    int N_drops = 0;
    printf("%s: %dms total\n", argv[1], (int)((double)N*1000/w.sr));
    for (int i = win; i < N; i++) {
        bool low = env[i] < avg[i] * 0.2f && avg[i] > 0.001f;
        if (low) {
            if (!inDrop) { dropStart = i; inDrop = 1; }
        } else {
            if (inDrop) {
                int len = i - dropStart;
                if (len >= minDropSamples) {
                    N_drops++;
                    if (N_drops <= 30) {
                        printf("  drop @ %.0fms len=%.1fms env=%.4f avg=%.4f\n",
                            (double)dropStart*1000.0/w.sr,
                            (double)len*1000.0/w.sr,
                            env[dropStart + len/2], avg[dropStart]);
                    }
                }
                inDrop = 0;
            }
        }
    }
    printf("total drops: %d\n", N_drops);
    return 0;
}
