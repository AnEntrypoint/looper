// test-wrapper-compile.cpp — verify RubberBandWrapper.h (with yinPsolaOctaver
// wired in for the -12 branch) compiles cleanly as a translation unit.
//
// Uses the in-tree patches/signalsmith stub which provides minimal STL shims
// for the firmware-target build, so this catches problems that g++ against
// host STL would miss.
//
// Compile:
//   g++ -std=c++17 -O2 -I patches scripts/test-wrapper-compile.cpp -c -o /tmp/wrap.o

#include "RubberBandWrapper.h"

extern "C" int main() {
    RubberBandWrapper w(48000, 2);
    w.setPitchScale(0.5f);
    int16_t l[64] = {0}, r[64] = {0};
    w.feedAudio(l, r, 64);
    w.retrieveAudio(l, r, 64);
    return 0;
}
