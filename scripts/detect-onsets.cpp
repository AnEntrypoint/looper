// Detect attack onsets in a WAV by envelope-derivative peak picking.
// Prints timestamps in ms for each detected onset.
//
// Compile: g++ -std=c++17 -O2 -o scripts/detect-onsets.exe scripts/detect-onsets.cpp
// Use: ./scripts/detect-onsets.exe path.wav

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
        } else {
            fseek(f, sz, SEEK_CUR);
        }
    }
    w.sr = sr; fclose(f);
    return w;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s wav\n", argv[0]); return 1; }
    Wav w = readWav(argv[1]);
    int N = (int)w.s.size();
    if (N == 0) { fprintf(stderr, "empty wav\n"); return 2; }

    // Compute envelope: full-wave rectify + 1-pole LPF (~5ms TC).
    std::vector<float> env(N);
    float e = 0;
    const float a = 1.0f - std::exp(-1.0f / (w.sr * 0.005f));
    for (int i = 0; i < N; i++) {
        float r = std::fabs(w.s[i]);
        e += (r - e) * a;
        env[i] = e;
    }
    // Derivative.
    std::vector<float> deriv(N);
    for (int i = 1; i < N; i++) deriv[i] = env[i] - env[i-1];
    // Threshold = some fraction of derivative max.
    float dmax = 0;
    for (float d : deriv) if (d > dmax) dmax = d;
    float thresh = dmax * 0.35f;
    int minSep = w.sr / 10;  // 100 ms min separation
    std::vector<int> onsets;
    int lastOnset = -minSep;
    for (int i = 1; i < N; i++) {
        if (deriv[i] > thresh && i - lastOnset > minSep) {
            onsets.push_back(i);
            lastOnset = i;
        }
    }
    printf("%s: %dms total, onsets:", argv[1], (int)((double)N*1000/w.sr));
    for (int o : onsets) printf(" %.0fms", (double)o*1000.0/w.sr);
    printf("\n");
    return 0;
}
