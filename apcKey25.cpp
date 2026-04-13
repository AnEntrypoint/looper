#define log_name "apc25"

#include "apcKey25.h"
#include "input_usb.h"
#include "usbMidi.h"
#include <circle/logger.h>
#include <circle/timer.h>

apcKey25 *pTheAPC = 0;

apcKey25::apcKey25() : m_shift(false), m_cmdReady(false), m_cmdType(ApcCmd::NONE), m_cmdArg(0), m_nowMs(0), m_bootMs(0), m_lastLedMs(0)
{
    pTheAPC = this;
    for (int i = 0; i < LOOPER_NUM_TRACKS; i++)
    {
        m_col1HoldStart[i]      = 0;
        m_col1Held[i]           = false;
        m_col1EraseTriggered[i] = false;
    }
}

u8 apcKey25::_padNote(int row, int col)
{
    return (u8)(row * APC_COLS + col);
}

void apcKey25::_sendLed(u8 note, u8 velocity)
{
    usbMidiSendNoteOn(note, velocity);
}

// Col 0: rec/play/overdub — red=recording, yellow=overdub or pending, green=playing
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

// Col 1: green=has clips unmuted, red=muted, off=empty
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
    _sendLed(APC_BTN_PLAY,     m_shift ? APC_VEL_LED_YELLOW : APC_VEL_LED_OFF);
}

// Safe to call from interrupt — only sets flags
void apcKey25::_queueCmd(ApcCmd::Type type, int arg)
{
    m_cmdType  = type;
    m_cmdArg   = arg;
    m_cmdReady = true;
}

void apcKey25::_onPadPress(int row, int col)
{
    if (row >= LOOPER_NUM_TRACKS) return;
    if (col == 0)
    {
        _queueCmd(ApcCmd::TRACK, row);
    }
    else if (col == 1)
    {
        m_col1Held[row]           = true;
        m_col1EraseTriggered[row] = false;
        m_col1HoldStart[row]      = m_nowMs;
    }
    else if (col >= 2 && col <= 5)
    {
        int layer = col - 2;
        _queueCmd(ApcCmd::LOOPER, LOOP_COMMAND_CLEAR_LAYER_BASE + row * LOOPER_NUM_LAYERS + layer);
    }
    else if (col == 6)
    {
        _queueCmd(ApcCmd::LOOPER, LOOP_COMMAND_HALVE_TRACK_BASE + row);
    }
    else if (col == 7)
    {
        _queueCmd(ApcCmd::LOOPER, LOOP_COMMAND_DOUBLE_TRACK_BASE + row);
    }
}

void apcKey25::_onPadRelease(int row, int col)
{
    if (col == 1 && row < LOOPER_NUM_TRACKS)
    {
        if (m_col1Held[row] && !m_col1EraseTriggered[row])
        {
            publicTrack *pTrack = pTheLooper->getPublicTrack(row);
            if (pTrack->getNumRunningClips())
                _queueCmd(ApcCmd::STOP_TRACK, row);
            else
                _queueCmd(ApcCmd::ERASE_TRACK, row);
        }
        m_col1Held[row] = false;
    }
}

void apcKey25::_onButton(u8 note)
{
    if (note == APC_BTN_STOP_ALL)
        _queueCmd(ApcCmd::LOOPER, m_shift ? LOOP_COMMAND_STOP_IMMEDIATE : LOOP_COMMAND_STOP);
    else if (note == APC_BTN_RECORD)
        _queueCmd(ApcCmd::LOOPER, m_shift ? LOOP_COMMAND_ABORT_RECORDING : LOOP_COMMAND_DUB_MODE);
    else if (note == APC_BTN_PLAY)
        _queueCmd(ApcCmd::LOOPER, m_shift ? LOOP_COMMAND_LOOP_IMMEDIATE : LOOP_COMMAND_CLEAR_ALL);
}

void apcKey25::handleMidi(u8 status, u8 data1, u8 data2)
{
    u8 msgType = status & 0xF0;

    if (msgType == APC_CH_NOTE_ON && data2 > 0)
    {
        if (data1 == APC_BTN_SHIFT) { m_shift = true; return; }
        if (data1 < APC_ROWS * APC_COLS)
        {
            _onPadPress(data1 / APC_COLS, data1 % APC_COLS);
            return;
        }
        _onButton(data1);
        return;
    }

    if (msgType == APC_CH_NOTE_OFF || (msgType == APC_CH_NOTE_ON && data2 == 0))
    {
        if (data1 == APC_BTN_SHIFT) { m_shift = false; return; }
        if (data1 < APC_ROWS * APC_COLS)
            _onPadRelease(data1 / APC_COLS, data1 % APC_COLS);
        return;
    }

    if (msgType == 0xB0 && data1 == 1)
    {
#ifdef LOOPER_LIVE_PITCH
        extern RubberBandWrapper *pLivePitchWrapper;
        if (pLivePitchWrapper)
        {
            float ratio = 0.5f + (data2 / 127.0f) * 2.0f;
            pLivePitchWrapper->setTempoRatio(ratio);
        }
#endif
        return;
    }
}

void apcKey25::update()
{
    if (!pTheLooper) return;

    m_nowMs = CTimer::Get()->GetClockTicks() / 1000;

    // Record boot time on first call
    if (m_bootMs == 0) m_bootMs = m_nowMs;

    // Dispatch queued command from interrupt
    if (m_cmdReady)
    {
        m_cmdReady = false;
        ApcCmd::Type type = m_cmdType;
        int arg = m_cmdArg;

        if (type == ApcCmd::TRACK)
        {
            u16 ts = pTheLooper->getPublicTrack(arg)->getTrackState();
            pTheLooper->command(LOOP_COMMAND_TRACK_BASE + arg);
        }
        else if (type == ApcCmd::STOP_TRACK)
        {
            pTheLooper->command(LOOP_COMMAND_STOP_TRACK_BASE + arg);
        }
        else if (type == ApcCmd::ERASE_TRACK)
        {
            pTheLooper->command(LOOP_COMMAND_ERASE_TRACK_BASE + arg);
        }
        else if (type == ApcCmd::LOOPER)
        {
            pTheLooper->command(arg);
        }
    }

    // Hold-to-erase on col1
    for (int row = 0; row < LOOPER_NUM_TRACKS; row++)
    {
        if (m_col1Held[row] && !m_col1EraseTriggered[row])
        {
            if (m_nowMs - m_col1HoldStart[row] >= APC_HOLD_ERASE_MS)
            {
                m_col1EraseTriggered[row] = true;
                m_col1Held[row] = false;
                pTheLooper->command(LOOP_COMMAND_ERASE_TRACK_BASE + row);
            }
        }
    }

    // Wait for APC25 to finish booting before sending LEDs
    if (m_nowMs - m_bootMs < APC_LED_BOOT_DELAY_MS) return;

    // Throttle LED updates to ~30fps
    if (m_nowMs - m_lastLedMs < 33) return;
    m_lastLedMs = m_nowMs;

    _updateGridLeds();
}
