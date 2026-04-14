#ifndef RUBBERBAND_RUBBERBAND_STRETCHER_H
#define RUBBERBAND_RUBBERBAND_STRETCHER_H

#include <stddef.h>
#include <string.h>
#include <math.h>

namespace RubberBand {

class RubberBandStretcher {
    static constexpr size_t BUF = 8192;
    static constexpr size_t MASK = BUF - 1;
    float m_buf[2][BUF];
    size_t m_wr;
    float  m_rd;
    size_t m_channels;
    double m_pitchScale;

public:
    enum Option {
        OptionProcessRealTime = 0x00000001,
        OptionEngineFaster    = 0x00000002,
        OptionWindowShort     = 0x00000100,
    };

    RubberBandStretcher(size_t, size_t channels, int, double, double)
        : m_wr(0), m_rd(0.0f), m_channels(channels), m_pitchScale(1.0)
    {
        memset(m_buf, 0, sizeof(m_buf));
    }
    ~RubberBandStretcher() {}

    void setMaxProcessSize(size_t) {}

    void process(const float *const *input, size_t samples, bool) {
        for (size_t i = 0; i < samples; i++) {
            m_buf[0][m_wr & MASK] = input[0][i];
            if (m_channels > 1) m_buf[1][m_wr & MASK] = input[1][i];
            m_wr++;
        }
    }

    int retrieve(float *const *output, size_t maxSamples) const {
        size_t avail = (size_t)available();
        if (avail > maxSamples) avail = maxSamples;
        auto *self = const_cast<RubberBandStretcher*>(this);
        for (size_t i = 0; i < avail; i++) {
            size_t i0 = (size_t)self->m_rd & MASK;
            size_t i1 = (i0 + 1) & MASK;
            float frac = self->m_rd - (float)(size_t)self->m_rd;
            output[0][i] = m_buf[0][i0] + frac * (m_buf[0][i1] - m_buf[0][i0]);
            if (m_channels > 1)
                output[1][i] = m_buf[1][i0] + frac * (m_buf[1][i1] - m_buf[1][i0]);
            self->m_rd += (float)m_pitchScale;
        }
        return (int)avail;
    }

    void setTimeRatio(double) {}
    void setPitchScale(double scale) { m_pitchScale = scale; }

    size_t getSamplesRequired() const { return 0; }

    int available() const {
        float filled = (float)m_wr - m_rd;
        if (filled < 0.0f) return 0;
        size_t avail = (size_t)filled;
        return avail > BUF/2 ? (int)(BUF/2) : (int)avail;
    }

    size_t getStartDelay() const { return 0; }
};

} // namespace RubberBand

#endif
