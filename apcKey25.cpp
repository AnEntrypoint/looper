#define log_name "apc25"

#include "apcKey25.h"
#include "input_usb.h"
#include "usbMidi.h"
#include <circle/logger.h>
#include <circle/timer.h>

apcKey25 *pTheAPC = 0;

apcKey25::apcKey25()
    : m_shift(false), m_cmdReady(false), m_cmdType(ApcCmd::NONE), m_cmdArg(0),
      m_nowMs(0), m_bootMs(0), m_lastLedMs(0),
      m_transposeLocked(false), m_transposePitch(0), m_pitchWheelOffset(0),
      m_driftTarget(0.0f), m_lastDriftMs(0), m_computedRatio(1.0f),
      m_liveEngaged(false), m_livePitchSemitones(0.0f), m_liveLedDirty(false),
      m_filterHP(0.0f), m_filterLP(1.0f), m_filterRes(0.0f),
      m_reverbAmount(0.0f), m_delayAmount(0.0f), m_time(0.5f), m_formant(0.0f)
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

void apcKey25::_queueCmd(ApcCmd::Type type, int arg)
{
    m_cmdType  = type;
    m_cmdArg   = arg;
    m_cmdReady = true;
}

void apcKey25::handleMidi(u8 status, u8 data1, u8 data2)
{
    u8 msgType = status & 0xF0;
    u8 channel = status & 0x0F;

    if (msgType == APC_CH_NOTE_ON && data2 > 0)
    {
        if (data1 == APC_BTN_SHIFT) { m_shift = true; return; }
        if (channel == 0 && data1 == 64) {
            m_liveEngaged = !m_liveEngaged;
            if (!m_liveEngaged) m_livePitchSemitones = 0.0f;
            _applyLivePitch();
            return;
        }
        if (channel == 1) {
            m_transposeLocked = true;
            return;
        }
        if (channel == 2) {
            m_livePitchSemitones = (float)((int)data1 - 60);
            m_liveEngaged = true;
            _applyLivePitch();
            return;
        }
        if (data1 < APC_ROWS * APC_COLS)
        {
            _onPadPress(data1 / APC_COLS, data1 % APC_COLS);
            return;
        }
        if (channel == 0 && data1 >= 48 && data1 <= 84)
        {
            m_livePitchSemitones = (float)((int)data1 - 60);
            m_liveEngaged = true;
            _applyLivePitch();
            return;
        }
        _onButton(data1);
        return;
    }

    if (msgType == APC_CH_NOTE_OFF || (msgType == APC_CH_NOTE_ON && data2 == 0))
    {
        if (data1 == APC_BTN_SHIFT) { m_shift = false; return; }
        if (channel == 0 && data1 == 64) return;
        if (channel == 1) { m_transposeLocked = false; return; }
        if (data1 < APC_ROWS * APC_COLS)
            _onPadRelease(data1 / APC_COLS, data1 % APC_COLS);
        return;
    }

    if (msgType == 0xB0 && data1 == 1)
    {
        bool inDeadzone = (data2 >= 59 && data2 <= 69);
        if (inDeadzone) {
            m_liveEngaged = false;
            m_livePitchSemitones = 0.0f;
        } else {
            m_livePitchSemitones = ((float)((int)data2 - 64)) * 6.0f / 63.0f;
            m_liveEngaged = true;
        }
        _applyLivePitch();
        return;
    }

    if (msgType == 0xB0 && data1 == 52)
    {
        m_livePitchSemitones = (data2 / 127.0f) * 12.0f - 6.0f;
        m_liveEngaged = true;
        _applyLivePitch();
        return;
    }

    if (msgType == 0xB0 && (data1 == 51 || data1 == 54 || data1 == 55))
    {
        handleFilterCC(data1, data2);
        return;
    }

    if (msgType == 0xB0 && (data1 == 48 || data1 == 49 || data1 == 50 || data1 == 53))
    {
        handleEffectsCC(data1, data2);
        return;
    }

}

void apcKey25::update()
{
    if (!pTheLooper) return;

    if (m_liveLedDirty)
    {
        m_liveLedDirty = false;
        usbMidiSend(0x90, 0x40, m_liveEngaged ? 127 : 0);
    }

    m_nowMs = CTimer::Get()->GetClockTicks() / 1000;

    if (m_bootMs == 0) m_bootMs = m_nowMs;

    if (m_cmdReady)
    {
        m_cmdReady = false;
        ApcCmd::Type type = m_cmdType;
        int arg = m_cmdArg;

        if (type == ApcCmd::TRACK)
        {
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
    _updateDrift();
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
    if (m_nowMs - m_bootMs < APC_LED_BOOT_DELAY_MS) return;
    if (m_nowMs - m_lastLedMs < 33) return;
    m_lastLedMs = m_nowMs;
    _updateGridLeds();
}