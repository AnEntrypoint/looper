// Find audible-click candidates: sample-to-sample differences above
// N*sigma of the rolling stddev. Prints timestamps of the worst ones.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

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
    if (argc < 2) { fprintf(stderr, "usage: %s wav\n", argv[0]); return 1; }
    Wav w = readWav(argv[1]);
    int N = (int)w.s.size();
    std::vector<float> d(N);
    for (int i = 1; i < N; i++) d[i] = w.s[i] - w.s[i-1];
    // Rolling stddev (~50ms window)
    int win = w.sr / 20;
    double sum2 = 0;
    for (int i = 0; i < win && i < N; i++) sum2 += d[i]*d[i];
    int N_click = 0;
    std::vector<std::pair<int,float>> clicks;
    for (int i = win; i < N; i++) {
        double rms = std::sqrt(sum2 / win);
        float mag = std::fabs(d[i]);
        if (rms > 1e-6 && mag > rms * 4.0f && mag > 0.001f) {
            clicks.push_back({i, mag / (float)rms});
            N_click++;
        }
        sum2 += d[i]*d[i] - d[i-win]*d[i-win];
    }
    // Print worst 20 clicks
    std::sort(clicks.begin(), clicks.end(),
        [](auto& a, auto& b){ return a.second > b.second; });
    printf("%s: %d total clicks; top 20:\n", argv[1], N_click);
    for (int i = 0; i < (int)clicks.size() && i < 20; i++) {
        printf("  %.0fms ratio=%.1fσ\n",
            (double)clicks[i].first * 1000.0 / w.sr, clicks[i].second);
    }
    return 0;
}
