#include "Looper.h"
#include <circle/logger.h>
#include <circle/synchronize.h>
#include "abletonLink.h"

#define log_name "lmachine"

#define WITH_VOLUMES       1
#define WITH_INT_VOLUMES   0

#if WITH_VOLUMES
    #if WITH_INT_VOLUMES

        #include <audio/utility/dspinst.h>

        #define MULTI_UNITYGAIN 65536

        static void applyGain(int16_t *data, int32_t mult)
        {
            // uint32_t tmp32 = *data; // read 2 samples from *data
            int32_t val1 = signed_multiply_32x16b(mult, *data);
            val1 = signed_saturate_rshift(val1, 16, 0);
            *data = val1;
        }
    #endif
#endif

// The default input gain for the cs42448 of 0db
// 0.723 = (128.0 / (128.0 + 1.0 + 48.0))
// 91.84 =  0.723 * 127
//
// My volumes are 100 based to allow for gain
// The output gain is limited to 1.0
//
// These values are emulated (owned) by the TeensyExpression pedal
// as it assumes initial values for the rotaries .... and zero
// for the loop volume pedal ...

u16 control_default[LOOPER_NUM_CONTROLS] = {
    94,     // codec input
    63,     // thru
    63,     // loop
    63,     // mix
    127};    // codec output defaults to 1.0


// 32 bit buffers for processing

s32 loopMachine::m_input_buffer[ LOOPER_NUM_CHANNELS * AUDIO_BLOCK_SAMPLES ];
s32 loopMachine::m_output_buffer[ LOOPER_NUM_CHANNELS * AUDIO_BLOCK_SAMPLES ];


//-------------------------------------------
// static externs
//-------------------------------------------


const char *getLoopCommandName(u16 cmd)
{
    if (cmd == LOOP_COMMAND_NONE)                 return "";                // function is disabled
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


//-------------------------------------------
// publicLoopMachine
//-------------------------------------------

publicLoopMachine::publicLoopMachine() :
   AudioStream(LOOPER_NUM_CHANNELS,LOOPER_NUM_CHANNELS,inputQueueArray)
{
    LOG("publicLoopMachine ctor",0);
    pCodec = AudioCodec::getSystemCodec();

	if (!pCodec)
		LOG_WARNING("No audio system codec!",0);
    // assert(pCodec);

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
    // hand out the head of the list
{
	DisableIRQs();	// in synchronize.h
    logString_t *retval =  m_pFirstLogString;
    if (retval)
    {
        m_pFirstLogString = retval->next;
        retval->next = 0;   // for safety
    }
    if (!m_pFirstLogString)
        m_pLastLogString = 0;
    EnableIRQs();
    return retval;
}


float publicLoopMachine::getMeter(u16 meter, u16 channel)
{
    // technically, interrupts should be turned off for this
    // and possible volatile declared here or there.
    meter_t *pm = &m_meter[meter];
	DisableIRQs();	// in synchronize.h
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
	// TE3 doesn't have a codec, but for sanity
	// I plug the INPUT and OUTPUT gains into the
	// m_control.values

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
        #if WITH_INT_VOLUMES
            //if (n > 32767.0f) n = 32767.0f;
            //else if (n < -32767.0f) n = -32767.0f;
            m_control[control].multiplier = scale * 65536.0f;
        #else
            scale = ((float)value)/63;
        #endif
    }
    m_control[control].value = value;
    m_control[control].scale = scale;
}



//--------------------------------------------------
// loopMachine
//--------------------------------------------------

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
    // initializes base class members too
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

    #if 0
        CString *msg;
        msg.FormatV(format,vars);
        va_end (vars);
        CLogger::Get()->Write(lname,LogDebug,msg);
    #else

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

    #endif
}



//-------------------------------------------------
// command processor
//-------------------------------------------------
// Note that the DUB button must be pressed BEFORE
// the given TRACK button, AND that it is cleared
// also on any accepting of a pending command


// virtual
void loopMachine::command(u16 command)
    // it is important to use LOOPER_LOG instead of LOG here
    // as this *may* be called from a serial interrupt, and
    // you don't want to serial output in the middle of a
    // serial input interrupt ... it causes "noise" in the
    // bcm_pcm machine.
{
    if (command == LOOP_COMMAND_NONE)
        return;

    LOOPER_LOG("loopMachine::command(%s)",getLoopCommandName(command));

    // immediate commands ...

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

	// recent addtions

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
				m_mark_point_state = 1;		// not in effect yet
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

	// ERASE_TRACKS

    else if (command >= LOOP_COMMAND_ERASE_TRACK_BASE &&
             command < LOOP_COMMAND_ERASE_TRACK_BASE + LOOPER_NUM_TRACKS)
    {
        int track_num = command - LOOP_COMMAND_ERASE_TRACK_BASE;
        LOOPER_LOG("LOOP_COMMAND_ERASE_TRACK(%d)",track_num);
        loopTrack *pTrack = getTrack(track_num);
        int num_running = pTrack->getNumRunningClips();

        // if the track has running clips, this is essentially a stop immediate

        if (num_running)
        {
            for (int i=0; i<LOOPER_NUM_TRACKS; i++)
                getTrack(i)->stopImmediate();
            m_running = 0;
            m_cur_track_num = -1;
            m_selected_track_num = -1;
            m_pending_command = 0;
        }

		// otherwise, if the track is the selected track this has the
		// effect cancelling any pending command and reverting to the
		// current running track, if any

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

    // STOP is a "pending" command

    else if (command == LOOP_COMMAND_STOP)
    {
        LOOPER_LOG("PENDING_STOP_COMMAND(%s)",getLoopCommandName(m_pending_command));
        m_pending_command = command;
    }

    // RC-505 style per-track independent button: each press cycles the track state.
    // No track selection — each track acts independently.

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

    }   // TRACK COMMAND


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

}   // loopMachine::command()




//----------------------------------------------
// Update()
//----------------------------------------------

inline s16 simple_clip(s32 val32)
{
	if (val32 > S32_MAX)
		return S32_MAX;
	if (val32 < S32_MIN)
		return S32_MIN;
	return val32;
}


// virtual
void loopMachine::update(void)
{
    m_cur_command = 0;
        // the current command is a short-lived member variable,
        // only valid for the duration of update.  It is "latched"
        // from the pending command at "loopPoints".

    //--------------------------------------------------------------
    // receive the input audio blocks and create the output blocks
    //--------------------------------------------------------------
    // loop through the samples setting the InputMeter min max,
    // applying the ThruControl, set the ThruMeter min and max,
    // and put the result in the output block.

    #if WITH_VOLUMES
        #if WITH_INT_VOLUMES
            int32_t thru_mult = m_control[LOOPER_CONTROL_THRU_VOLUME].multiplier;
            int32_t loop_mult = m_control[LOOPER_CONTROL_LOOP_VOLUME].multiplier;
            int32_t mix_mult = m_control[LOOPER_CONTROL_MIX_VOLUME].multiplier;
        #else
            float thru_level = m_control[LOOPER_CONTROL_THRU_VOLUME].scale;
            float loop_level = m_control[LOOPER_CONTROL_LOOP_VOLUME].scale;
            float mix_level =  m_control[LOOPER_CONTROL_MIX_VOLUME].scale;
        #endif
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

			*poi++ = val;		// copy to 32 bit input buffer

		}	// for each sample

		if (in)
			AudioSystem::release(in);

	}   // for each s32 channel

	if (linkIsSynced() && m_running == 0)
	{
		double bpm = linkGetBPM();
		if (bpm > 0) m_masterLoopBlocks = (u32)((INTEGRAL_BLOCKS_PER_SECOND * 60.0) / bpm + 0.5);
	}
	updateState();
	if (m_running)
	{
		if (m_masterLoopBlocks > 0)
			m_masterPhase = (m_masterPhase + 1) % m_masterLoopBlocks;
		for (int i=0; i<LOOPER_NUM_TRACKS; i++)
		{
			loopTrack *pTrack = getTrack(i);
			if (pTrack->getNumRunningClips())
			{
				pTrack->update(m_input_buffer,m_output_buffer);
			}
		}
	}   // m_running

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

			// apply Thru and Loop Control volume level
			// note use of doubles for USE_32BIT_MIX

			#if WITH_VOLUMES
				#if WITH_INT_VOLUMES
					applyGain(&ival,thru_mult);			// will not compile
					applyGain(&oval,loop_mult);
				#else
					ival32 = ((double)ival32) * thru_level;
					oval32 = ((double)oval32) * loop_level;
				#endif
			#endif

			// add them to create the Mix value
			// and apply it's volume if'defd

			s32 mval32 = ival32 + oval32;

			#if WITH_VOLUMES
				#if WITH_INT_VOLUMES
					applyGain(&mval,mix_mult);		// will not compile
			   #else
					mval32 = ((double)mval32) * mix_level;
				#endif
			#endif

			// apply simple clipping

			s16 ival = simple_clip(ival32);
			s16 oval = simple_clip(oval32);
			s16 mval = simple_clip(mval32);

			// update the meters in 16 bit-land

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

			// place the 16 bit sample in the output buffer

			*op++ = mval;

		}   // for each input sample

		// transmit the output blocks
		// and release all blocks

		transmit(out[channel], channel);
		AudioSystem::release(out[channel]);
	}

    m_cur_command = 0;
        // end of short lived command variable

}   // loopMachine::update()



//--------------------------------------------------------------------------------
// updateState()
//--------------------------------------------------------------------------------


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
    // handle STOP_IMMEDIATE
    // bring everything to a screeching halt

    //----------------------------------------
    // updateState() COMMAND processor.
    //----------------------------------------
    // Determine if we are at a "loop point" or a point
    // at which we should latch the pending command into
    // the current command ...

	loopTrack *pCurTrack = m_cur_track_num >= 0 ?  getTrack(m_cur_track_num) : 0;
	loopClip  *pCurClip0 = pCurTrack ? pCurTrack->getClip(0) : 0;
	ClipState cur_clip0_state = pCurClip0 ? pCurClip0->getClipState() : CS_IDLE;

	loopTrack *pSelTrack = m_selected_track_num >=0 ? getTrack(m_selected_track_num) : 0;
	// loopClip  *pSelClip0 = pSelTrack ? pSelTrack->getClip(0) : 0;
	// u16 sel_clip0_state = pSelClip0 ? pSelClip0->getClipState() : 0;

		// the current base clip, and it's state, if any
	bool at_loop_point = (cur_clip0_state == CS_PLAYING || cur_clip0_state == CS_LOOPING) && !pCurClip0->getPlayBlockNum();

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

    }   // if m_pending_command

    // RC-505: process per-track pending commands independently
    for (int i = 0; i < LOOPER_NUM_TRACKS; i++)
    {
        if (!m_track_pending[i]) continue;

        loopTrack *pTrack = getTrack(i);
        loopClip  *pClip0 = pTrack->getClip(0);
        ClipState clip0_state = pClip0 ? pClip0->getClipState() : CS_IDLE;

        bool at_phrase_start = (m_masterLoopBlocks > 0) && (m_masterPhase == 0);
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
            CLogger::Get()->Write(log_name, LogNotice, "TRACK(%d) latching cmd=%d", i, cmd);
            pTrack->updateState(cmd);
        }
    }

}   // loopMachine::updateState()
