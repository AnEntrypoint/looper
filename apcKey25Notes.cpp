#define log_name "apc25"

#include "apcKey25.h"

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
    else if (note == APC_BTN_FORMAT)
    {
        m_liveEngaged = !m_liveEngaged;
        _applyLivePitch();
    }
}
