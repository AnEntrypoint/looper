#include "Looper.h"
#include <circle/logger.h>

#define log_name "lclip"

CString *getClipStateName(ClipState s)
{
    CString *msg = new CString();
    switch (s)
    {
        case CS_IDLE:           msg->Append("IDLE"); break;
        case CS_RECORDING:      msg->Append("RECORDING"); break;
        case CS_RECORDING_MAIN: msg->Append("RECORDING_MAIN"); break;
        case CS_RECORDING_TAIL: msg->Append("RECORDING_TAIL"); break;
        case CS_FINISHING:      msg->Append("FINISHING"); break;
        case CS_RECORDED:       msg->Append("RECORDED"); break;
        case CS_PLAYING:        msg->Append("PLAYING"); break;
        case CS_LOOPING:        msg->Append("LOOPING"); break;
        case CS_STOPPING:       msg->Append("STOPPING"); break;
        default:                msg->Append("UNKNOWN"); break;
    }
    return msg;
}

u32 loopClip::_calcQuantizeTarget()
{
    u32 masterLen = pTheLoopMachine->m_masterLoopBlocks;
    if (masterLen == 0) return m_record_block;
    u32 n = m_record_block / masterLen;
    if (n == 0) n = 1;
    return n * masterLen;
}

void loopClip::updateState(u16 cur_command)
{
    LOOPER_LOG("clip(%d,%d) updateState(%s) state=%d", m_track_num, m_clip_num, getLoopCommandName(cur_command), (int)m_state);

    if (cur_command == LOOP_COMMAND_LOOP_IMMEDIATE)
    {
        switch (m_state)
        {
            case CS_RECORDING:
            case CS_RECORDING_MAIN:
            case CS_RECORDING_TAIL:
            case CS_FINISHING:
                stopImmediate();
                break;
            case CS_PLAYING:
                if (m_play_block) _startCrossFade();
                break;
            default:
                break;
        }
    }
    else if (cur_command == LOOP_COMMAND_SET_LOOP_START)
    {
        m_mark_point_active = 1;
        if (m_state == CS_PLAYING && m_play_block) _startCrossFade();
    }
    else if (cur_command == LOOP_COMMAND_STOP)
    {
        if (m_state == CS_RECORDING_MAIN)
        {
            u32 target = _calcQuantizeTarget();
            if (target <= m_record_block)
                _startEndingRecording(target, false);
            else
            {
                m_quantizeTarget = target;
                m_state = CS_FINISHING;
            }
        }
        else if (m_state == CS_PLAYING)
        {
            if (m_play_block)
                _startFadeOut();
            else
            {
                m_state = CS_RECORDED;
                m_play_block = 0;
                m_pLoopTrack->incDecRunning(-1);
            }
        }
    }
    else if (cur_command == LOOP_COMMAND_PLAY)
    {
        if (m_state == CS_RECORDING || m_state == CS_RECORDING_MAIN)
        {
            if (m_state == CS_RECORDING && m_record_block == 0)
            {
                stopImmediate();
                return;
            }
            u32 target = _calcQuantizeTarget();
            if (target <= m_record_block)
                _startEndingRecording(target, true);
            else
            {
                m_quantizeTarget = target;
                m_state = CS_RECORDING_TAIL;
            }
        }
        if (m_state == CS_RECORDED)
            _startPlaying();
    }
    else if (cur_command == LOOP_COMMAND_RECORD)
    {
        _startRecording();
    }
}
