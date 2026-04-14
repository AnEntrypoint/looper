#ifndef RUBBERBAND_RUBBERBAND_STRETCHER_H
#define RUBBERBAND_RUBBERBAND_STRETCHER_H

#include <stddef.h>
#include <string.h>

namespace RubberBand {

class RubberBandStretcher {
    static constexpr size_t BUF = 8192;
    float m_buf[2][BUF];
    size_t m_wr, m_rd, m_channels;

public:
    enum Option {
        OptionProcessRealTime = 0x00000001,
        OptionEngineFaster    = 0x00000002,
        OptionWindowShort     = 0x00000100,
    };

    RubberBandStretcher(size_t, size_t channels, int, double, double)
        : m_wr(0), m_rd(0), m_channels(channels)
    {
        memset(m_buf, 0, sizeof(m_buf));
    }
    ~RubberBandStretcher() {}

    void setMaxProcessSize(size_t) {}

    void process(const float *const *input, size_t samples, bool) {
        for (size_t i = 0; i < samples; i++) {
            size_t pos = (m_wr + i) % BUF;
            m_buf[0][pos] = input[0][i];
            if (m_channels > 1) m_buf[1][pos] = input[1][i];
        }
        m_wr = (m_wr + samples) % BUF;
    }

    int retrieve(float *const *output, size_t maxSamples) const {
        size_t avail = (m_wr + BUF - m_rd) % BUF;
        if (avail > maxSamples) avail = maxSamples;
        for (size_t i = 0; i < avail; i++) {
            size_t pos = (m_rd + i) % BUF;
            output[0][i] = m_buf[0][pos];
            if (m_channels > 1) output[1][i] = m_buf[1][pos];
        }
        const_cast<RubberBandStretcher*>(this)->m_rd = (m_rd + avail) % BUF;
        return (int)avail;
    }

    void setTimeRatio(double) {}
    void setPitchScale(double) {}
    size_t getSamplesRequired() const { return 0; }
    int    available() const {
        return (int)((m_wr + BUF - m_rd) % BUF);
    }
    size_t getStartDelay() const { return 0; }
};

} // namespace RubberBand

#endif
