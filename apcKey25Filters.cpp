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
        // Formant depth knob. Default range ±1 (musical territory).
        // Hold SHIFT while turning to expand to ±3 (extreme / special-fx).
        // Center deadzone (data2 60-68) snaps to 0 = natural pitch shift
        // (formants slide with pitch). Above center → formants preserved
        // at original pitch (vocal-octave character). Below center →
        // formants doubled-down (huge/monster).
        bool inDeadzone = (data2 >= 60 && data2 <= 68);
        float range = m_shift ? 3.0f : 1.0f;
        m_formant = inDeadzone ? 0.0f
                               : (((float)((int)data2 - 64)) / 63.0f) * range;
        _applyFormant();
    }
    // CC56/57 (formant resonance + peak freq) were used by the prior
    // SincFormantOctaver post-EQ — solad-snac uses formant DEPTH instead,
    // so those knobs are unmapped now. They remain on the APC hardware
    // but produce no audio change.
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
