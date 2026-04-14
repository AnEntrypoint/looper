#ifndef RUBBERBAND_RUBBERBAND_STRETCHER_H
#define RUBBERBAND_RUBBERBAND_STRETCHER_H

#include <stddef.h>

namespace RubberBand {

class RubberBandStretcher {
public:
    enum Option {
        OptionProcessRealTime = 0x00000001,
        OptionEngineFaster    = 0x00000002,
        OptionWindowShort     = 0x00000100,
    };

    RubberBandStretcher(size_t, size_t, int, double, double) {}
    ~RubberBandStretcher() {}

    void setMaxProcessSize(size_t) {}
    void process(const float *const *, size_t, bool) {}
    int  retrieve(float *const *, size_t) const { return 0; }
    void setTimeRatio(double) {}
    void setPitchScale(double) {}
    size_t getSamplesRequired() const { return 0; }
    int    available() const { return 0; }
    size_t getStartDelay() const { return 0; }
};

} // namespace RubberBand

#endif
