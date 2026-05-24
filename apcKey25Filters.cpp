#define log_name "apc25"

#include "apcKey25.h"
#include "usbMidi.h"
#include "patches/apcEffectsProcessor.h"
#include "patches/RubberBandWrapper.h"
#include <math.h>

extern apcEffectsProcessor *pEffectsProcessor;
extern RubberBandWrapper *pLivePitchWrapper;

void apcKey25::_applyFilters()
{
    if (pEffectsProcessor) {
        pEffectsProcessor->setHighpassCutoff(m_filterHP);
        pEffectsProcessor->setLowpassCutoff(m_filterLP);
        pEffectsProcessor->setLowpassResonance(m_filterRes);
    }
}

void apcKey25::handleFilterCC(u8 cc, u8 data2)
{
    float norm = data2 / 127.0f;
    if (cc == 51)
    {
        m_filterHP = norm;
        _applyFilters();
    }
    else if (cc == 54)
    {
        m_filterRes = norm;
        _applyFilters();
    }
    else if (cc == 55)
    {
        m_filterLP = norm;
        _applyFilters();
    }
}

void apcKey25::handleEffectsCC(u8 cc, u8 data2)
{
    float norm = data2 / 127.0f;
    if (cc == 48)
    {
        m_reverbAmount = norm;
        _applyEffects();
    }
    else if (cc == 49)
    {
        m_delayAmount = norm;
        _applyEffects();
    }
    else if (cc == 50)
    {
        m_time = norm;
        _applyEffects();
    }
    else if (cc == 53)
    {
        // Brightness ∈ [-1, +1] centred at data2=64 (mod-wheel deadzone behavior).
        bool inDeadzone = (data2 >= 60 && data2 <= 68);
        m_formant = inDeadzone ? 0.0f : (((float)((int)data2 - 64)) / 63.0f);
        _applyFormant();
    }
    else if (cc == 56)
    {
        // Resonance ∈ [0, 1] linear from data2.
        m_formantResonance = norm;
        _applyFormant();
    }
    else if (cc == 57)
    {
        // Peak frequency 300..3000 Hz log-mapped from data2 ∈ [0, 127].
        // f = 300 * (3000/300)^(data2/127) = 300 * 10^(data2/127)
        float t = (float)data2 / 127.0f;
        m_formantFreq = 300.0f * powf(10.0f, t);
        _applyFormant();
    }
}

void apcKey25::_applyEffects()
{
    if (pEffectsProcessor) {
        pEffectsProcessor->setReverbAmount(m_reverbAmount);
        pEffectsProcessor->setDelayAmount(m_delayAmount);
        pEffectsProcessor->setTime(m_time);
    }
}

void apcKey25::_applyFormant()
{
    if (pLivePitchWrapper) {
        // Three-knob path: drives both the sinc octaver post-EQ (-12 branch)
        // and the signalsmith formant factor (other ratios) via brightness.
        pLivePitchWrapper->setFormantEq(m_formant, m_formantResonance, m_formantFreq);
        pLivePitchWrapper->setFormant(m_formant);
    }
}
