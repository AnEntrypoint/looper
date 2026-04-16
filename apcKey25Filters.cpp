#define log_name "apc25"

#include "apcKey25.h"
#include "usbMidi.h"
#include "patches/apcEffectsProcessor.h"
#include "patches/RubberBandWrapper.h"

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
        m_formant = norm;
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
        pLivePitchWrapper->setFormant(m_formant);
    }
}
