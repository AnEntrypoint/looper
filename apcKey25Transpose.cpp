#define log_name "apc25"

#include "apcKey25.h"
#include "input_usb.h"
#include "Looper.h"

extern apcKey25 *pTheAPC;
extern publicLoopMachine *pTheLooper;

void apcKey25::_updateComputedRatio()
{
    float semitones = (float)m_transposePitch + m_pitchWheelOffset;
    m_computedRatio = pow(2.0f, semitones / 12.0f);
}

void apcKey25::_updateDrift()
{
    if (m_pitchWheelOffset != m_driftTarget)
    {
        unsigned long elapsedMs = m_nowMs - m_lastDriftMs;
        float driftPerMs = 0.01f;
        float drift = driftPerMs * elapsedMs;
        if (m_pitchWheelOffset > m_driftTarget + 0.5f)
            m_pitchWheelOffset -= drift;
        else if (m_pitchWheelOffset < m_driftTarget - 0.5f)
            m_pitchWheelOffset += drift;
        else
            m_pitchWheelOffset = m_driftTarget;
        m_lastDriftMs = m_nowMs;
        _updateComputedRatio();
    }
}

u8 apcKey25::_trackLedColor(int track)
{
    if (track >= LOOPER_NUM_TRACKS) return APC_VEL_LED_OFF;
    publicTrack *pTrack = pTheLooper->getPublicTrack(track);
    u16 ts = pTrack->getTrackState();

    if (ts & TRACK_STATE_RECORDING)
        return pTrack->getNumRecordedClips() > 0 ? APC_VEL_LED_YELLOW : APC_VEL_LED_RED;
    if (ts & (TRACK_STATE_PENDING_RECORD | TRACK_STATE_PENDING_PLAY | TRACK_STATE_PENDING_STOP))
        return APC_VEL_LED_YELLOW;
    if (ts & TRACK_STATE_PLAYING)
        return APC_VEL_LED_GREEN;
    return APC_VEL_LED_OFF;
}

u8 apcKey25::_muteLedColor(int track)
{
    if (track >= LOOPER_NUM_TRACKS) return APC_VEL_LED_OFF;
    publicTrack *pTrack = pTheLooper->getPublicTrack(track);
    int layers = pTrack->getNumRecordedClips();
    if (layers == 0) return APC_VEL_LED_OFF;
    bool stopped = (pTrack->getTrackState() & TRACK_STATE_STOPPED) != 0;
    u8 color = APC_VEL_LED_GREEN;
    if (layers >= 3) color = APC_VEL_LED_RED;
    else if (layers >= 2) color = APC_VEL_LED_YELLOW;
    if (stopped) color++;
    return color;
}

void apcKey25::_updateGridLeds()
{
    for (int row = 0; row < LOOPER_NUM_TRACKS; row++)
    {
        u8 col0 = _trackLedColor(row);
        u8 col1 = _muteLedColor(row);
        _sendLed(_padNote(row, 0), col0);
        _sendLed(_padNote(row, 1), col1);
    }

    for (int track = 0; track < LOOPER_NUM_TRACKS; track++)
    {
        int col = 2 + track;
        if (col >= APC_COLS - 1) break;
        publicTrack *pTrack = pTheLooper->getPublicTrack(track);
        u32 tpeak = pTrack->m_peakLevel;
        pTrack->m_peakLevel = 0;
        int tvu = 0;
        if (tpeak > 50)    tvu = 1;
        if (tpeak > 200)   tvu = 2;
        if (tpeak > 1000)  tvu = 3;
        if (tpeak > 4000)  tvu = 4;
        if (tpeak > 10000) tvu = 5;
        for (int row = 0; row < APC_ROWS; row++)
        {
            u8 color = APC_VEL_LED_OFF;
            if (row < tvu)
                color = (row >= 4) ? APC_VEL_LED_RED : APC_VEL_LED_GREEN;
            _sendLed(_padNote(row, col), color);
        }
    }

    u32 peak = AudioInputUSB::s_peakLevel;
    AudioInputUSB::s_peakLevel = 0;
    int inVu = 0;
    if (peak > 100)   inVu = 1;
    if (peak > 500)   inVu = 2;
    if (peak > 2000)  inVu = 3;
    if (peak > 5000)  inVu = 4;
    if (peak > 10000) inVu = 5;

    u32 outPeak = pTheLooper->m_outputPeakLevel;
    pTheLooper->m_outputPeakLevel = 0;
    int outVu = 0;
    if (outPeak > 50)    outVu = 1;
    if (outPeak > 200)   outVu = 2;
    if (outPeak > 1000)  outVu = 3;
    if (outPeak > 4000)  outVu = 4;
    if (outPeak > 10000) outVu = 5;

    for (int i = 0; i < 5; i++)
    {
        u8 color = APC_VEL_LED_OFF;
        if (i < inVu)
            color = (i >= 4) ? APC_VEL_LED_RED : APC_VEL_LED_GREEN;
        else if (i < outVu)
            color = (i >= 4) ? APC_VEL_LED_RED : APC_VEL_LED_YELLOW;
        _sendLed(0x52 + i, color);
    }

    bool running = pTheLooper->getRunning();
    u16  pending = pTheLooper->getPendingCommand();
    _sendLed(APC_BTN_STOP_ALL, running ? (pending == LOOP_COMMAND_STOP ? APC_VEL_LED_YELLOW : APC_VEL_LED_GREEN) : APC_VEL_LED_OFF);
    _sendLed(APC_BTN_RECORD,   pTheLooper->getDubMode() ? APC_VEL_LED_RED : APC_VEL_LED_OFF);
    _sendLed(APC_BTN_PLAY,     pTheAPC->m_shift ? APC_VEL_LED_YELLOW : APC_VEL_LED_OFF);
}
