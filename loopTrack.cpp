#include "Looper.h"
#include <circle/logger.h>
#include <circle/util.h>

#define log_name "ltrack"


// static
CString *getTrackStateName(u16 track_state)
{
	CString *msg = new CString();

    if (track_state == TRACK_STATE_EMPTY)
        msg->Append("EMPTY ");
    if (track_state & TRACK_STATE_RECORDING)
        msg->Append("RECORDING ");
    if (track_state & TRACK_STATE_PLAYING)
        msg->Append("PLAYING ");
    if (track_state & TRACK_STATE_STOPPED)
        msg->Append("STOPPED ");
    if (track_state & TRACK_STATE_PENDING_RECORD)
        msg->Append("PEND_RECORD ");
    if (track_state & TRACK_STATE_PENDING_PLAY)
        msg->Append("PEND_PLAY ");
    if (track_state & TRACK_STATE_PENDING_STOP)
        msg->Append("PEND_STOP ");
    return msg;
}




// virtual

int loopTrack::getTrackState()
{
    int state = 0;
    if (m_num_used_clips == 0)
        state |= TRACK_STATE_EMPTY;

    for (int i=0; i<m_num_used_clips; i++)
    {
        loopClip *pClip = m_clips[i];
        int clip_state = pClip->getClipState();
        if (clip_state & (CLIP_STATE_RECORD_IN | CLIP_STATE_RECORD_MAIN))
            state |= TRACK_STATE_RECORDING;
        if (clip_state & CLIP_STATE_PLAY_MAIN)
            state |= TRACK_STATE_PLAYING;
    }

    if (m_num_used_clips && !(state & (TRACK_STATE_RECORDING | TRACK_STATE_PLAYING)))
        state |= TRACK_STATE_STOPPED;

    u16 pending = pTheLoopMachine->getPendingCommand();
    u16 sel_num = pTheLoopMachine->getSelectedTrackNum();

    if (pending && m_track_num == sel_num)
    {
        if (pending == LOOP_COMMAND_STOP)
            state |= TRACK_STATE_PENDING_STOP;
        else if (pending == LOOP_COMMAND_RECORD)
            state |= TRACK_STATE_PENDING_RECORD;
        else if (pending == LOOP_COMMAND_PLAY)
            state |= TRACK_STATE_PENDING_PLAY;
    }
    return state;
}


loopTrack::loopTrack(u16 track_num) : publicTrack(track_num)
{
    m_track_num = track_num;
    for (int i=0; i<LOOPER_NUM_LAYERS; i++)
    {
        m_clips[i] = new loopClip(i,this);
    }
    init();
}


loopTrack::~loopTrack()
{
    for (int i=0; i<LOOPER_NUM_LAYERS; i++)
    {
        delete m_clips[i];
        m_clips[i] = 0;
    }
}



void loopTrack::init()
{
    publicTrack::init();
    for (int i=0; i<LOOPER_NUM_LAYERS; i++)
        m_clips[i]->init();
}




void loopTrack::clearMarkPoint()
{
	LOOPER_LOG("clearMarkPoint()",0);
	for (int i=0; i<m_num_used_clips; i++)
	{
		m_clips[i]->clearMarkPoint();
	}
}
void loopTrack::setMarkPoint()
{
	LOOPER_LOG("setMarkPoint()",0);
	DisableIRQs();	// in synchronize.h
	for (int i=0; i<m_num_used_clips; i++)
		m_clips[i]->setMarkPoint();
	EnableIRQs();	// in synchronize.h
}



void loopTrack::update(s32 *in, s32 *out)
{
    u32 peak = 0;
    s32 snap[LOOPER_NUM_CHANNELS * AUDIO_BLOCK_SAMPLES];
    memcpy(snap, out, sizeof snap);

    for (int i=0; i<m_num_used_clips; i++)
        m_clips[i]->update(in,out);

    for (int i = 0; i < LOOPER_NUM_CHANNELS * AUDIO_BLOCK_SAMPLES; i++)
    {
        s32 diff = out[i] - snap[i];
        u32 abs = diff < 0 ? (u32)(-diff) : (u32)diff;
        if (abs > peak) peak = abs;
    }
    if (peak > m_peakLevel) m_peakLevel = peak;
}



void loopTrack::incDecNumUsedClips(int inc)
{
    m_num_used_clips += inc;
    LOOPER_LOG("track(%d) inDecNumUsedClips(%d)=%d",m_track_num,inc,m_num_used_clips);
}
void loopTrack::incDecNumRecordedClips(int inc)
{
    m_num_recorded_clips += inc;
    LOOPER_LOG("track(%d) incDecNumRecordedClips(%d)=%d",m_track_num,inc,m_num_recorded_clips);
}
void loopTrack::incDecRunning(int inc)
{
    m_num_running_clips += inc;
    LOOPER_LOG("track(%d) incDecRunning(%d) m_num_running_clips=%d",m_track_num,inc,m_num_running_clips);
    pTheLoopMachine->incDecRunning(inc);

}



void loopTrack::stopImmediate()
{
    for (int i=0; i<m_num_used_clips; i++)
    {
        m_clips[i]->stopImmediate();
    }
    m_num_running_clips = 0;
}


void loopTrack::clearClip(int layer)
{
    if (layer < 0 || layer >= m_num_used_clips) return;
    loopClip *pClip = m_clips[layer];
    if (pClip->getClipState() & (CLIP_STATE_PLAY_MAIN | CLIP_STATE_PLAY_END))
        incDecRunning(-1);
    pClip->init();
    if (layer < m_num_recorded_clips)
        m_num_recorded_clips--;
    m_num_used_clips--;
    for (int i = layer; i < LOOPER_NUM_LAYERS - 1; i++)
    {
        loopClip *tmp = m_clips[i];
        m_clips[i] = m_clips[i+1];
        m_clips[i+1] = tmp;
    }
}

void loopTrack::halveLength()
{
    for (int i = 0; i < m_num_used_clips; i++)
        m_clips[i]->halveLength();
}

void loopTrack::doubleLength()
{
    for (int i = 0; i < m_num_used_clips; i++)
        m_clips[i]->doubleLength();
}


void loopTrack::updateState(u16 cur_command)
{
    LOOPER_LOG("track(%d) updateState(%s)",m_track_num,getLoopCommandName(cur_command));
    CLogger::Get()->Write(log_name, LogNotice, "track(%d) updateState cmd=%d used=%d rec=%d run=%d", m_track_num, cur_command, m_num_used_clips, m_num_recorded_clips, m_num_running_clips);

    if (cur_command == LOOP_COMMAND_STOP ||
        cur_command == LOOP_COMMAND_PLAY ||
		cur_command == LOOP_COMMAND_LOOP_IMMEDIATE ||
		cur_command == LOOP_COMMAND_SET_LOOP_START)
    {
        for (int i=0; i<m_num_used_clips; i++)
        {
            m_clips[i]->updateState(cur_command);
        }
    }
    else if (cur_command == LOOP_COMMAND_RECORD)
    {
        for (int i=0; i<m_num_used_clips; i++)
        {
            m_clips[i]->updateState(LOOP_COMMAND_PLAY);
                // the command play on the recording clip
                // *may* cause it to stop recording, and
                // increment m_num_recorded_clips
        }
        if (m_num_recorded_clips < LOOPER_NUM_LAYERS)
        {
            m_clips[m_num_recorded_clips]->updateState(LOOP_COMMAND_RECORD);
        }
    }
}
