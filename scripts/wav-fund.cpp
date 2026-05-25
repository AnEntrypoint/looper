// Report the dominant fundamental (20-400 Hz) of a 16-bit mono/stereo WAV
// via autocorrelation with parabolic interpolation. The -12 pitch witness.
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cstring>
#include <cmath>

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: wav-fund <file.wav>\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open\n"); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz); fread(buf.data(), 1, sz, f); fclose(f);
    // parse fmt + data chunks
    int sr = 48000, ch = 1, bits = 16; long dataOff = 44, dataLen = sz - 44;
    for (long p = 12; p + 8 <= sz;) {
        uint32_t id; memcpy(&id, &buf[p], 4);
        uint32_t cl; memcpy(&cl, &buf[p+4], 4);
        if (memcmp(&buf[p], "fmt ", 4) == 0) {
            ch  = *(int16_t*)&buf[p+8+2];
            sr  = *(int32_t*)&buf[p+8+4];
            bits= *(int16_t*)&buf[p+8+14];
        } else if (memcmp(&buf[p], "data", 4) == 0) {
            dataOff = p + 8; dataLen = cl; break;
        }
        p += 8 + cl + (cl & 1);
    }
    int bytesPerSamp = bits / 8;
    long nFrames = dataLen / (bytesPerSamp * ch);
    std::vector<float> x(nFrames);
    const int16_t* s = (const int16_t*)&buf[dataOff];
    for (long i = 0; i < nFrames; i++) x[i] = s[i*ch] / 32768.0f;  // ch 0

    int minLag = sr / 400, maxLag = sr / 20;
    long start = nFrames / 3;
    std::vector<double> ac(maxLag + 2, 0.0);
    double best = -1e30; int bl = minLag;
    for (int lag = minLag; lag <= maxLag; lag++) {
        double a = 0.0;
        for (long i = start; i < nFrames - lag; i++) a += (double)x[i]*x[i+lag];
        ac[lag] = a;
        if (a > best) { best = a; bl = lag; }
    }
    double y0 = ac[bl-1], y1 = ac[bl], y2 = ac[bl+1];
    double den = y0 - 2*y1 + y2;
    double d = den != 0 ? 0.5*(y0-y2)/den : 0.0;
    double fund = (double)sr / (bl + d);
    // rms
    double rms = 0; for (long i=start;i<nFrames;i++) rms += x[i]*x[i]; rms = sqrt(rms/(nFrames-start));
    printf("file=%s sr=%d ch=%d frames=%ld dominant_fund=%.2f Hz rms=%.5f\n",
           argv[1], sr, ch, nFrames, fund, rms);
    return 0;
}
