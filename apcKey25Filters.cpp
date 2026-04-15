#define log_name "apc25"

#include "apcKey25.h"
#include "usbMidi.h"

void apcKey25::_applyFilters()
{
    // TODO: Wire to effects processor when signalsmith compatibility resolved
    // if (pEffectsProcessor) {
    //   pEffectsProcessor->setHighpassCutoff(m_filterHP);
    //   pEffectsProcessor->setLowpassCutoff(m_filterLP);
    //   pEffectsProcessor->setFilterResonance(m_filterRes);
    // }
}

void apcKey25::handleFilterCC(u8 cc, u8 data2)
{
    if (cc == 51)
    {
        m_filterLP = data2 / 127.0f;
        _applyFilters();
    }
    else if (cc == 54)
    {
        m_filterHP = data2 / 127.0f;
        _applyFilters();
    }
    else if (cc == 55)
    {
        m_filterRes = data2 / 127.0f;
        _applyFilters();
    }
}

void apcKey25::handleEffectsCC(u8 cc, u8 data2)
{
    if (cc == 48)
    {
        m_reverbAmount = data2 / 127.0f;
        _applyEffects();
    }
    else if (cc == 49)
    {
        m_reverbTime = data2 / 127.0f;
        _applyEffects();
    }
    else if (cc == 50)
    {
        m_delayAmount = data2 / 127.0f;
        _applyEffects();
    }
    else if (cc == 53)
    {
        m_delayTime = data2 / 127.0f;
        _applyEffects();
    }
}

void apcKey25::_applyEffects()
{
    if (!pEffectsProcessor) return;
    pEffectsProcessor->setReverbAmount(m_reverbAmount);
    pEffectsProcessor->setReverbTime(m_reverbTime);
    pEffectsProcessor->setDelayAmount(m_delayAmount);
    pEffectsProcessor->setDelayTime(m_delayTime);
}
