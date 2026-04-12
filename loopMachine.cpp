#include "Looper.h"
#include <circle/logger.h>
#include <circle/synchronize.h>
#include "abletonLink.h"

#define log_name "lmachine"

#define WITH_VOLUMES       1


u16 control_default[LOOPER_NUM_CONTROLS] = {
    94,
    63,
    63,
    63,
    127};


s32 loopMachine::m_input_buffer[ LOOPER_NUM_CHANNELS * AUDIO_BLOCK_SAMPLES ];
s32 loopMachine::m_output_buffer[ LOOPER_NUM_CHANNELS * AUDIO_BLOCK_SAMPLES ];


const char *getLoopCommandName(u16 cmd)
{
    if (cmd == LOOP_COMMAND_NONE)                 return "";
    if (cmd == LOOP_COMMAND_CLEAR_ALL)            return "CLEAR";
    if (cmd == LOOP_COMMAND_STOP_IMMEDIATE)       return "STOP!";
    if (cmd == LOOP_COMMAND_STOP)                 return "STOP";
    if (cmd == LOOP_COMMAND_ABORT_RECORDING)      return "ABORT";
    if (cmd == LOOP_COMMAND_DUB_MODE)             return "DUB";
	if (cmd == LOOP_COMMAND_LOOP_IMMEDIATE)		  return "LOOP!";
	if (cmd == LOOP_COMMAND_SET_LOOP_START)       return "SET_START";
	if (cmd == LOOP_COMMAND_CLEAR_LOOP_START)     return "CLR_START";
    if (cmd == LOOP_COMMAND_TRACK_BASE+0)         return "TRACK1";
    if (cmd == LOOP_COMMAND_TRACK_BASE+1)         return "TRACK2";
    if (cmd == LOOP_COMMAND_TRACK_BASE+2)         return "TRACK3";
    if (cmd == LOOP_COMMAND_TRACK_BASE+3)         return "TRACK4";
    if (cmd == LOOP_COMMAND_ERASE_TRACK_BASE+0)   return "ETRK0";
    if (cmd == LOOP_COMMAND_ERASE_TRACK_BASE+1)   return "ETRK1";
    if (cmd == LOOP_COMMAND_ERASE_TRACK_BASE+2)   return "ETRK2";
    if (cmd == LOOP_COMMAND_ERASE_TRACK_BASE+3)   return "ETRK3";
    if (cmd == LOOP_COMMAND_PLAY)                 return "PLAY";
    if (cmd == LOOP_COMMAND_RECORD)               return "REC";
    return "UNKNOWN_LOOP_COMMAND";
}


publicLoopMachine::publicLoopMachine() :
   AudioStream(LOOPER_NUM_CHANNELS,LOOPER_NUM_CHANNELS,inputQueueArray)
{
    LOG("publicLoopMachine ctor",0);
    pCodec = AudioCodec::getSystemCodec();

	if (!pCodec)
		LOG_WARNING("No audio system codec!",0);

    m_pFirstLogString = 0;
    m_pLastLogString = 0;

    for (int i=0; i<LOOPER_NUM_CONTROLS; i++)
    {
        m_control[i].value = 0;
        m_control[i].default_value = control_default[i];
        m_control[i].scale = 0.00;
        m_control[i].multiplier = 0;
    }
    for (int i=0; i<NUM_METERS; i++)
    {
        for (int j=0; j<LOOPER_NUM_CHANNELS; j++)
        {
            m_meter[i].min_sample[j] = 0;
            m_meter[i].max_sample[j] = 0;
        }
    }

    init();

    LOG("publicLoopMachine ctor finished",0);
}


publicLoopMachine::~publicLoopMachine()
{
    pCodec = 0;
}


logString_t *publicLoopMachine::getNextLogString()
{
	DisableIRQs();
    logString_t *retval =  m_pFirstLogString;
    if (retval)
    {
        m_pFirstLogString = retval->next;
        retval->next = 0;
    }
    if (!m_pFirstLogString)
        m_pLastLogString = 0;
    EnableIRQs();
    return retval;
}


float publicLoopMachine::getMeter(u16 meter, u16 channel)
{
    meter_t *pm = &m_meter[meter];
	DisableIRQs();
    int min = - pm->min_sample[channel];
    int max = pm->max_sample[channel];
    pm->max_sample[channel] = 0;
    pm->min_sample[channel] = 0;
    EnableIRQs();
    if (min > max) max = min;
    return (float)max / 32767.0f;
}


u8 publicLoopMachine::getControlValue(u16 control)
{
    return m_control[control].value;
}

u8 publicLoopMachine::getControlDefault(u16 control)
{
    return m_control[control].default_value;
}


void publicLoopMachine::setControl(u16 control, u8 value)
{

    float scale = ((float)value)/127.00;
    if (control == LOOPER_CONTROL_INPUT_GAIN)
    {
        if (pCodec)
			pCodec->inputLevel(scale);
		else
			m_control[control].value = value;

    }
    else if (control == LOOPER_CONTROL_OUTPUT_GAIN)
    {
        if (pCodec)
			pCodec->volume(scale);
		else
		    m_control[control].value = value;

    }
    else
    {
            scale = ((float)value)/63;
    }
    m_control[control].value = value;
    m_control[control].scale = scale;
}


loopMachine::loopMachine() : publicLoopMachine()
{
    LOG("loopMachine ctor",0);

    new loopBuffer();

    for (int i=0; i<LOOPER_NUM_TRACKS; i++)
    {
        m_tracks[i] = new loopTrack(i);
    }

    init();

    LOG("loopMachine ctor finished",0);
}


loopMachine::~loopMachine()
{
    if (pTheLoopBuffer)
        delete pTheLoopBuffer;
    pTheLoopBuffer = 0;
    for (int i=0; i<LOOPER_NUM_TRACKS; i++)
    {
        if (m_tracks[i])
            delete  m_tracks[i];
        m_tracks[i] = 0;
    }
}


void loopMachine::init()
{
    publicLoopMachine::init();

    m_cur_command = 0;
    m_cur_track_num = -1;
	m_mark_point_state = 0;

    for (int i = 0; i < LOOPER_NUM_TRACKS; i++)
        m_track_pending[i] = LOOP_COMMAND_NONE;

    pTheLoopBuffer->init();

    for (int i=0; i<LOOPER_NUM_TRACKS; i++)
    {
        m_tracks[i]->init();
    }
}


void loopMachine::LogUpdate(const char *lname, const char *format, ...)
{
	va_list vars;
	va_start(vars, format);


        logString_t *pMem = new logString_t;
        pMem->next = 0;
        pMem->lname = lname;
        pMem->string = new CString();
        pMem->string->FormatV(format,vars);
        va_end (vars);

        if (!m_pFirstLogString)
            m_pFirstLogString = pMem;
        if (m_pLastLogString)
            m_pLastLogString->next = pMem;

        m_pLastLogString = pMem;

}


void loopMachine::command(u16 command)
{
    if (command == LOOP_COMMAND_NONE)
        return;

    LOOPER_LOG("loopMachine::command(%s)",getLoopCommandName(command));


    if (command == LOOP_COMMAND_STOP_IMMEDIATE)
    {
        LOOPER_LOG("LOOP_COMMAND_STOP_IMMEDIATE",0);

        for (int i=0; i<LOOPER_NUM_TRACKS; i++)
        {
            loopTrack *pT = getTrack(i);
            if (pT->getNumRunningClips())
                pT->stopImmediate();
        }

        m_running = 0;
        m_cur_track_num = -1;
        m_selected_track_num = -1;
        m_pending_command = 0;
    }
    else if (command == LOOP_COMMAND_ABORT_RECORDING)
    {
        LOOPER_LOG("LOOP_COMMAND_ABORT_RECORDING m_cur_track_num=%d",m_cur_track_num);
		if (m_cur_track_num >= 0)
		{
			loopTrack *pCurTrack = getTrack(m_cur_track_num);
			if (pCurTrack->getTrackState() & TRACK_STATE_RECORDING)
			{
				loopClip *pClip = pCurTrack->getClip(pCurTrack->getNumRecordedClips());
				pClip->stopImmediate();
			}
		}
    }

    else if (command == LOOP_COMMAND_CLEAR_ALL)
    {
        LOOPER_LOG("LOOP_COMMAND_CLEAR",0);
        init();
    }
    else if (command == LOOP_COMMAND_DUB_MODE)
    {
        LOOPER_LOG("DUB_MODE=%d",m_dub_mode);
        m_dub_mode = !m_dub_mode;
    }


    else if (command == LOOP_COMMAND_LOOP_IMMEDIATE)
    {
        LOOPER_LOG("LOOP_COMMAND_LOOP_IMMEDIATE()",0);
        m_pending_command = command;
    }
    else if (command == LOOP_COMMAND_SET_LOOP_START)
    {
        LOOPER_LOG("LOOP_COMMAND_SET_LOOP_START()",0);
		if (m_cur_track_num>=0)
		{
			if (m_tracks[m_cur_track_num]->getTrackState() & TRACK_STATE_PLAYING)
			{
				m_tracks[m_cur_track_num]->setMarkPoint();
				m_mark_point_state = 1;
			}
			else
			{
				LOOPER_LOG("ERROR - attempt to setMarkPoint on non-playing track",0);
			}
		}
		else
		{
			LOOPER_LOG("ERROR - attempt to setMarkPoint without current track",0);
		}

    }
    else if (command == LOOP_COMMAND_CLEAR_LOOP_START)
    {
        LOOPER_LOG("LOOP_COMMAND_CLEAR_LOOP_START()",0);
		m_mark_point_state = 0;
		if (m_cur_track_num>=0)
			m_tracks[m_cur_track_num]->clearMarkPoint();
    }


    else if (command >= LOOP_COMMAND_ERASE_TRACK_BASE &&
             command < LOOP_COMMAND_ERASE_TRACK_BASE + LOOPER_NUM_TRACKS)
    {
        int track_num = command - LOOP_COMMAND_ERASE_TRACK_BASE;
        LOOPER_LOG("LOOP_COMMAND_ERASE_TRACK(%d)",track_num);
        loopTrack *pTrack = getTrack(track_num);
        int num_running = pTrack->getNumRunningClips();


        if (num_running)
        {
            for (int i=0; i<LOOPER_NUM_TRACKS; i++)
                getTrack(i)->stopImmediate();
            m_running = 0;
            m_cur_track_num = -1;
            m_selected_track_num = -1;
            m_pending_command = 0;
        }


        else if (track_num == m_selected_track_num)
        {
            m_pending_command = 0;
            if (m_selected_track_num != -1)
				m_tracks[m_selected_track_num]->setSelected(false);
            if (m_running)
			{
				m_selected_track_num = m_cur_track_num;
				m_tracks[m_selected_track_num]->setSelected(true);
			}
			else
				m_selected_track_num = -1;
        }

        pTrack->init();

        bool anyClips = false;
        for (int i = 0; i < LOOPER_NUM_TRACKS; i++)
            if (getTrack(i)->getNumRecordedClips() > 0) { anyClips = true; break; }
        if (!anyClips)
        {
            m_masterLoopBlocks = 0;
            m_masterPhase = 0;
        }
    }


    else if (command == LOOP_COMMAND_STOP)
    {
        LOOPER_LOG("PENDING_STOP_COMMAND(%s)",getLoopCommandName(m_pending_command));
        m_pending_command = command;
    }


    else if (command >= LOOP_COMMAND_TRACK_BASE &&
             command < LOOP_COMMAND_TRACK_BASE + LOOPER_NUM_TRACKS)
    {
        int track_num = command - LOOP_COMMAND_TRACK_BASE;
        loopTrack *pTrack = getTrack(track_num);
        int ts = pTrack->getTrackState();

        if (ts & TRACK_STATE_PENDING_RECORD)
        {
            m_track_pending[track_num] = LOOP_COMMAND_NONE;
        }
        else
        {
            u16 next_cmd = LOOP_COMMAND_NONE;
            if (ts & TRACK_STATE_RECORDING)
                next_cmd = LOOP_COMMAND_PLAY;
            else if (ts & TRACK_STATE_PLAYING)
                next_cmd = LOOP_COMMAND_RECORD;
            else if (ts & TRACK_STATE_STOPPED)
                next_cmd = LOOP_COMMAND_PLAY;
            else
                next_cmd = LOOP_COMMAND_RECORD;

            if (next_cmd == LOOP_COMMAND_RECORD && m_masterLoopBlocks > 0)
                m_track_pending[track_num] = LOOP_COMMAND_RECORD;
            else
                pTrack->updateState(next_cmd);
        }

    }


    else if (command >= LOOP_COMMAND_STOP_TRACK_BASE &&
             command < LOOP_COMMAND_STOP_TRACK_BASE + LOOPER_NUM_TRACKS)
    {
        int track_num = command - LOOP_COMMAND_STOP_TRACK_BASE;
        getTrack(track_num)->updateState(LOOP_COMMAND_STOP);
    }

    else if (command >= LOOP_COMMAND_CLEAR_LAYER_BASE &&
             command < LOOP_COMMAND_CLEAR_LAYER_BASE + LOOPER_NUM_TRACKS * LOOPER_NUM_LAYERS)
    {
        int idx = command - LOOP_COMMAND_CLEAR_LAYER_BASE;
        int track_num = idx / LOOPER_NUM_LAYERS;
        int layer = idx % LOOPER_NUM_LAYERS;
        LOOPER_LOG("CLEAR_LAYER track=%d layer=%d", track_num, layer);
        if (track_num < LOOPER_NUM_TRACKS)
        {
            loopTrack *pTrack = getTrack(track_num);
            if (pTrack->getNumUsedClips() == 0)
            {
                int ts = pTrack->getTrackState();
                if (ts & TRACK_STATE_PENDING_RECORD)
                {
                    m_track_pending[track_num] = LOOP_COMMAND_NONE;
                }
                else
                {
                    u16 next_cmd = LOOP_COMMAND_RECORD;
                    if (next_cmd == LOOP_COMMAND_RECORD && m_masterLoopBlocks > 0)
                        m_track_pending[track_num] = LOOP_COMMAND_RECORD;
                    else
                        pTrack->updateState(next_cmd);
                }
            }
            else
            {
                pTrack->clearClip(layer);
                bool anyClips = false;
                for (int i = 0; i < LOOPER_NUM_TRACKS; i++)
                    if (getTrack(i)->getNumRecordedClips() > 0) { anyClips = true; break; }
                if (!anyClips)
                {
                    m_masterLoopBlocks = 0;
                    m_masterPhase = 0;
                }
            }
        }
    }

    else if (command >= LOOP_COMMAND_HALVE_TRACK_BASE &&
             command < LOOP_COMMAND_HALVE_TRACK_BASE + LOOPER_NUM_TRACKS)
    {
        int track_num = command - LOOP_COMMAND_HALVE_TRACK_BASE;
        LOOPER_LOG("HALVE_TRACK(%d)", track_num);
        getTrack(track_num)->halveLength();
    }

    else if (command >= LOOP_COMMAND_DOUBLE_TRACK_BASE &&
             command < LOOP_COMMAND_DOUBLE_TRACK_BASE + LOOPER_NUM_TRACKS)
    {
        int track_num = command - LOOP_COMMAND_DOUBLE_TRACK_BASE;
        LOOPER_LOG("DOUBLE_TRACK(%d)", track_num);
        getTrack(track_num)->doubleLength();
    }

}


inline s16 simple_clip(s32 val32)
{
	if (val32 > S32_MAX)
		return S32_MAX;
	if (val32 < S32_MIN)
		return S32_MIN;
	return val32;
}


void loopMachine::update(void)
{
    m_cur_command = 0;


    #if WITH_VOLUMES
            float thru_level = m_control[LOOPER_CONTROL_THRU_VOLUME].scale;
            float loop_level = m_control[LOOPER_CONTROL_LOOP_VOLUME].scale;
            float mix_level =  m_control[LOOPER_CONTROL_MIX_VOLUME].scale;
    #endif

	s32 *poi = m_input_buffer;
	audio_block_t *out[LOOPER_NUM_CHANNELS];
	memset(m_output_buffer,0,LOOPER_NUM_CHANNELS * AUDIO_BLOCK_SAMPLES *sizeof(s32));

	for (u16 channel=0; channel<LOOPER_NUM_CHANNELS; channel++)
	{
		audio_block_t *in = receiveReadOnly(channel);
		s16 *ip = in ? in->data : 0;
		out[channel] = AudioSystem::allocate();

		for (u16 i=0; i<AUDIO_BLOCK_SAMPLES; i++)
		{
			s16 val = ip ? *ip++ : 0;

			#if WITH_METERS
				s16 *in_max   = &(m_meter[METER_INPUT].max_sample[channel]  );
				s16 *in_min   = &(m_meter[METER_INPUT].min_sample[channel]  );
					if (val > *in_max)
						*in_max = val;
					if (val <*in_min)
						*in_min = val;
			#endif

			*poi++ = val;

		}

		if (in)
			AudioSystem::release(in);

	}

	if (linkIsSynced())
	{
		double bpm = linkGetBPM();
		if (bpm > 0)
		{
			u32 raw = (u32)((INTEGRAL_BLOCKS_PER_SECOND * 60.0 * 16.0) / bpm + 0.5);
			u32 blocks = ((raw + 4) / 8) * 8;
			if (blocks != m_masterLoopBlocks)
			{
				m_masterLoopBlocks = blocks;
				LOOPER_LOG("link quantum: bpm=%.1f masterLoopBlocks=%u", bpm, blocks);
			}
		}
	}
	updateState();
	if (m_running)
	{
		m_masterPhase++;
		for (int i=0; i<LOOPER_NUM_TRACKS; i++)
		{
			loopTrack *pTrack = getTrack(i);
			if (pTrack->getNumRunningClips())
			{
				pTrack->update(m_input_buffer,m_output_buffer);
			}
		}
	}

	u32 outPeak = 0;
	for (int i = 0; i < LOOPER_NUM_CHANNELS * AUDIO_BLOCK_SAMPLES; i++)
	{
		u32 abs = m_output_buffer[i] < 0 ? (u32)(-m_output_buffer[i]) : (u32)m_output_buffer[i];
		if (abs > outPeak) outPeak = abs;
	}
	if (outPeak > m_outputPeakLevel) m_outputPeakLevel = outPeak;

	s32 *iip = m_input_buffer;
	s32 *iop = m_output_buffer;

	for (u16 channel=0; channel<LOOPER_NUM_CHANNELS; channel++)
	{
		s16 *op = out[channel]->data;

		#if WITH_METERS
			s16 *thru_max   = &(m_meter[METER_THRU].max_sample[channel]);
			s16 *thru_min   = &(m_meter[METER_THRU].min_sample[channel]);
			s16 *loop_max   = &(m_meter[METER_LOOP].max_sample[channel]);
			s16 *loop_min   = &(m_meter[METER_LOOP].min_sample[channel]);
			s16 *mix_max    = &(m_meter[METER_MIX].max_sample[channel]);
			s16 *mix_min    = &(m_meter[METER_MIX].min_sample[channel]);
		#endif

		for (u16 i=0; i<AUDIO_BLOCK_SAMPLES; i++)
		{
			s32 ival32 = *iip++;
			s32 oval32 = *iop++;


			#if WITH_VOLUMES
					ival32 = ((double)ival32) * thru_level;
					oval32 = ((double)oval32) * loop_level;
			#endif


			s32 mval32 = ival32 + oval32;

			#if WITH_VOLUMES
					mval32 = ((double)mval32) * mix_level;
			#endif


			s16 ival = simple_clip(ival32);
			s16 oval = simple_clip(oval32);
			s16 mval = simple_clip(mval32);


			#if WITH_METERS
				if (ival > *thru_max)
					*thru_max = ival;
				if (ival < *thru_min)
					*thru_min = ival;
				if (oval > *loop_max)
					*loop_max = oval;
				if (oval <*loop_min)
					*loop_min = oval;
				if (mval > *mix_max)
					*mix_max = ival;
				if (mval <*mix_min)
					*mix_min = mval;
			#endif


			*op++ = mval;

		}


		transmit(out[channel], channel);
		AudioSystem::release(out[channel]);
	}

    m_cur_command = 0;

}


void loopMachine::incDecRunning(int inc)
{
    m_running += inc;
    LOOPER_LOG("incDecRunning(%d) m_running=%d",inc,m_running);
    if (m_selected_track_num >=0 && !m_running)
    {
	    LOOPER_LOG("incDecRunning DESELECTING TRACK(%d)",m_selected_track_num);
        m_tracks[m_selected_track_num]->setSelected(false);
        m_selected_track_num = -1;
    }
}


void loopMachine::updateState(void)
{


	loopTrack *pCurTrack = m_cur_track_num >= 0 ?  getTrack(m_cur_track_num) : 0;
	loopClip  *pCurClip0 = pCurTrack ? pCurTrack->getClip(0) : 0;
	ClipState cur_clip0_state = pCurClip0 ? pCurClip0->getClipState() : CS_IDLE;

	loopTrack *pSelTrack = m_selected_track_num >=0 ? getTrack(m_selected_track_num) : 0;

	bool at_loop_point = (cur_clip0_state == CS_PLAYING || cur_clip0_state == CS_LOOPING) &&
		(m_masterLoopBlocks > 0 ? (m_masterPhase % m_masterLoopBlocks == 0) : !pCurClip0->getPlayBlockNum());

	if (at_loop_point)
	{
		m_pending_loop_notify++;
		if (m_mark_point_state && !m_pending_command)
		{
			LOOPER_LOG("forcing m_pending_command=LOOP_COMMAND_SET_LOOP_START",0);
			m_pending_command = LOOP_COMMAND_SET_LOOP_START;
		}
	}

    if (m_pending_command)
    {
        bool latch_command =
            !m_running ||
            at_loop_point ||
            (cur_clip0_state == CS_RECORDING_MAIN) ||
			m_pending_command == LOOP_COMMAND_LOOP_IMMEDIATE;

        if (latch_command)
        {
            m_cur_command = m_pending_command;
            m_pending_command = 0;

            LOOPER_LOG("latching global command(%s)",getLoopCommandName(m_cur_command));

            m_dub_mode = false;
        }

    }

    for (int i = 0; i < LOOPER_NUM_TRACKS; i++)
    {
        if (!m_track_pending[i]) continue;

        loopTrack *pTrack = getTrack(i);
        loopClip  *pClip0 = pTrack->getClip(0);
        ClipState clip0_state = pClip0 ? pClip0->getClipState() : CS_IDLE;

        bool at_phrase_start = (m_masterLoopBlocks > 0) && (m_masterPhase % m_masterLoopBlocks == 0);
        bool track_latch;
        if (m_track_pending[i] == LOOP_COMMAND_RECORD && m_masterLoopBlocks > 0)
            track_latch = at_phrase_start;
        else
            track_latch =
                !pTrack->getNumRunningClips() ||
                (clip0_state == CS_PLAYING || clip0_state == CS_LOOPING) && !pClip0->getPlayBlockNum() ||
                (clip0_state == CS_RECORDING_MAIN);

        if (track_latch)
        {
            u16 cmd = m_track_pending[i];
            m_track_pending[i] = LOOP_COMMAND_NONE;
            LOOPER_LOG("TRACK(%d) latching %s", i, getLoopCommandName(cmd));
            pTrack->updateState(cmd);
        }
    }

}
