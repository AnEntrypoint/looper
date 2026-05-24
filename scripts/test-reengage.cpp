// Test engine re-engage behavior.
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include "engines/engine_solad_snac.h"

int main() {
    const int SR = 48000;
    EngineSoladSnac e;
    e.setPitchScale(0.5f);
    {
        std::vector<float> in(SR), out(SR);
        for (int i = 0; i < SR; i++) in[i] = std::sin(2.0 * 3.14159265 * 200 * i / SR);
        const int CHUNK = 64;
        for (size_t i = 0; i < in.size(); i += CHUNK) {
            int n = (int)std::min((size_t)CHUNK, in.size() - i);
            e.processBlock(&in[i], &out[i], n);
        }
        printf("phase1 (-12) last samples: ");
        for (int i = SR - 8; i < SR; i++) printf("%.3f ", out[i]);
        printf("\n");
    }
    printf("phase2: simulating 0.5s disengage\n");
    e.setPitchScale(1.0f);
    {
        int N = SR * 2;
        std::vector<float> in(N), out(N);
        for (int i = 0; i < N; i++) in[i] = std::sin(2.0 * 3.14159265 * 200 * i / SR);
        const int CHUNK = 64;
        for (size_t i = 0; i < in.size(); i += CHUNK) {
            int n = (int)std::min((size_t)CHUNK, in.size() - i);
            e.processBlock(&in[i], &out[i], n);
        }
        for (int t : {100, 500, 1000, 1500, 1900}) {
            int idx = SR * t / 1000;
            double amp = 0;
            for (int i = idx; i < idx + 256; i++) amp += out[i] * out[i];
            amp = std::sqrt(amp / 256);
            double w = 2.0 * 3.14159265 * 200.0 / SR;
            double cc = 2.0 * std::cos(w);
            double q1 = 0, q2 = 0;
            for (int i = idx; i < idx + SR/10; i++) {
                double q0 = cc * q1 - q2 + out[i];
                q2 = q1; q1 = q0;
            }
            double mag = std::sqrt(q1*q1 + q2*q2 - q1*q2*cc) / (SR/20.0);
            printf("phase3 t=%4dms: rms=%.4f fund@200Hz=%.4f\n", t, amp, mag);
        }
    }
    return 0;
}
