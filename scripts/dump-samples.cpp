// Print N raw int16 samples starting at given offset (in ms) — for inspecting
// waveform shape around suspected glitch positions.
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
    if (argc < 3) { fprintf(stderr, "usage: %s wav offsetMs [count=64]\n", argv[0]); return 1; }
    Wav w = readWav(argv[1]);
    int N = (int)w.s.size();
    int off = (int)(std::atof(argv[2]) * w.sr / 1000.0);
    int cnt = argc > 3 ? std::atoi(argv[3]) : 64;
    if (off < 0) off = 0;
    if (off + cnt > N) cnt = N - off;
    printf("%s at %.3fms (sample %d): %d samples\n",
        argv[1], (double)off*1000.0/w.sr, off, cnt);
    for (int i = 0; i < cnt; i++) {
        int16_t v = w.s[off + i];
        printf("%6d ", v);
        if ((i+1) % 16 == 0) printf("\n");
    }
    if (cnt % 16) printf("\n");
    return 0;
}
