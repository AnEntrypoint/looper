#define log_name "apc25"

#include "apcKey25.h"
#include "usbMidi.h"

void apcKey25::_applyFilters()
{
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
