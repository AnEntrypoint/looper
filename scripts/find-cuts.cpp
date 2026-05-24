// Detect audio CUTS: runs of near-silence INSIDE an otherwise active signal.
// Tuned for very low absolute levels (our capture is at -70 dBFS) and small
// gap durations (audible cut = ~1ms or longer).
// Prints all cuts with timestamps + durations.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
struct Wav { int sr; std::vector<int16_t> s; };
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
                w.s[i] = l;
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
    if (N == 0) { fprintf(stderr, "empty wav\n"); return 2; }

    // Find absolute peak to normalise the threshold against actual signal level.
    int peak = 0;
    for (int i = 0; i < N; i++) { int a = w.s[i] < 0 ? -w.s[i] : w.s[i]; if (a > peak) peak = a; }
    if (peak < 3) { printf("signal too quiet (peak=%d)\n", peak); return 0; }

    // Track instantaneous abs-value with a short window. A "cut" = a window
    // of ≥1ms where every sample is below 10%% of recent local peak.
    int wsamp = w.sr / 1000;     // 1ms window
    int lookback = w.sr / 50;    // 20ms recent-peak window
    int cuts = 0;
    int inCut = 0, cutStart = 0;
    int gapMin = wsamp;
    printf("%s: %dms total, signal peak=%d (~%.1fdBFS)\n",
        argv[1], (int)((double)N*1000/w.sr), peak, 20.0*std::log10((double)peak/32768.0));
    for (int i = lookback; i + wsamp < N; i++) {
        // Recent local peak (max abs in last 20ms — represents the "expected" signal)
        int localPeak = 0;
        for (int j = i - lookback; j < i; j++) {
            int a = w.s[j] < 0 ? -w.s[j] : w.s[j];
            if (a > localPeak) localPeak = a;
        }
        if (localPeak < 2) continue;  // nothing to compare against
        // Is the next 1ms all near zero compared to local peak?
        int thresh = localPeak / 10;  // -20dB below local peak
        if (thresh < 1) thresh = 1;
        bool allLow = true;
        for (int j = 0; j < wsamp; j++) {
            int a = w.s[i+j] < 0 ? -w.s[i+j] : w.s[i+j];
            if (a > thresh) { allLow = false; break; }
        }
        if (allLow) {
            if (!inCut) { cutStart = i; inCut = 1; }
        } else {
            if (inCut) {
                int len = i - cutStart;
                if (len >= gapMin) {
                    cuts++;
                    if (cuts <= 40)
                        printf("  cut @ %.1fms len=%.1fms localPeak=%d\n",
                            (double)cutStart*1000.0/w.sr, (double)len*1000.0/w.sr,
                            localPeak);
                }
                inCut = 0;
            }
        }
    }
    printf("total cuts: %d\n", cuts);
    return 0;
}
