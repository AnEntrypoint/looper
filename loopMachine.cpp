#include "Looper.h"
#include <circle/logger.h>
#include <circle/synchronize.h>
#include "abletonLink.h"
#include "continuousBuffer.h"
#include "patches/RubberBandWrapper.h"
#include "patches/apcEffectsProcessor.h"
#include "patches/paramSnapshot.h"
#include "apcKey25.h"
extern RubberBandWrapper *pLivePitchWrapper;
extern apcEffectsProcessor *pEffectsProcessor;

// Record-latch grid witnesses for the :4445 TIME verb. Published when a
// deferred RECORD latches on the beat grid (Core 1), read by the verb (Core 2).
volatile u32 g_lastGridStep   = 0;
volatile u32 g_lastLatchPhase = 0;

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
    cbInit();   // heap-allocate the continuous rolling record buffer (~16MB)

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
    m_prevMasterLoopBlocks = 0;

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

        // Hold = stop+clear THIS pad unconditionally and leave it empty (LED
        // off). Unlike CLEAR_LAYER (which re-arms a pending RECORD on an empty
        // track when a master grid exists), ERASE_TRACK NEVER arms: it cancels
        // any queued deferred-record for this track, stops+inits the track, and
        // never sets pending. This is the load-bearing difference that makes a
        // long-hold always end with the LED off.
        m_track_pending[track_num] = LOOP_COMMAND_NONE;

        // Drop this track's selection so a stale selected-pending can't relight.
        if (track_num == m_selected_track_num)
        {
            m_pending_command = 0;
            m_tracks[track_num]->setSelected(false);
            m_selected_track_num = m_running ? m_cur_track_num : -1;
            if (m_selected_track_num != -1)
                m_tracks[m_selected_track_num]->setSelected(true);
        }

        // Clear every used layer via clearClip (which decrements the machine
        // running-count for any PLAYING/LOOPING/STOPPING clip, unlike a raw
        // init() that would leak the running count). Scoped to THIS track only
        // — a single-pad hold must not stop the whole rig. Clear from the top
        // down so the clearClip compaction shift stays in-bounds.
        for (int layer = pTrack->getNumUsedClips() - 1; layer >= 0; layer--)
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

	LiveParams lp = paramSnapshotLoad();
	if (pLivePitchWrapper)
	{
		// Gated engine — wrapper only runs when transpose is actively
		// engaged. When disengaged, the audio path bypasses the wrapper
		// entirely: zero added latency, zero engine CPU cost, zero risk of
		// pre-resample drift snaps producing periodic clicks. The
		// "constant-latency on engage/disengage" experiment regressed
		// passthrough stability (periodic ring-mod cuts when disengaged);
		// any smooth-engage feel needed lives inside the wrapper, not by
		// running the engine on silence.
		static float s_lastSemis = 0.0f;
		static bool  s_lastEngaged = false;
		float wantSemis = lp.liveEngaged ? lp.livePitchSemitones : 0.0f;
		if (wantSemis != s_lastSemis || lp.liveEngaged != s_lastEngaged) {
			float scale = lp.liveEngaged ? powf(2.0f, wantSemis / 12.0f) : 1.0f;
			pLivePitchWrapper->setPitchScale(scale);
			// Drive the wrapper's engaged flag — on false→true it triggers
			// reengage() to realign the engine readers, and it gates the
			// engine introspection telemetry. Was dropped when the gated
			// path was restored; without it isEngaged() stays false and
			// the re-align never fires.
			pLivePitchWrapper->setEngaged(lp.liveEngaged);
			s_lastSemis   = wantSemis;
			s_lastEngaged = lp.liveEngaged;
		}
		if (lp.liveEngaged)
		{
			s16 tmp_L[AUDIO_BLOCK_SAMPLES], tmp_R[AUDIO_BLOCK_SAMPLES];
			for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
			{
				tmp_L[i] = (s16)m_input_buffer[i];
				tmp_R[i] = (s16)m_input_buffer[AUDIO_BLOCK_SAMPLES + i];
			}

			pLivePitchWrapper->feedAudio(tmp_L, tmp_R, AUDIO_BLOCK_SAMPLES);
			s16 out_L[AUDIO_BLOCK_SAMPLES], out_R[AUDIO_BLOCK_SAMPLES];
			size_t got = pLivePitchWrapper->retrieveAudio(out_L, out_R, AUDIO_BLOCK_SAMPLES);
			if (got == AUDIO_BLOCK_SAMPLES)
			{
				for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
				{
					m_input_buffer[i] = out_L[i];
					m_input_buffer[AUDIO_BLOCK_SAMPLES + i] = out_R[i];
				}
			}
		}
	}

	if (pEffectsProcessor)
	{
		float fx_L[AUDIO_BLOCK_SAMPLES], fx_R[AUDIO_BLOCK_SAMPLES];
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		{
			fx_L[i] = (float)m_input_buffer[i] / 32768.0f;
			fx_R[i] = (float)m_input_buffer[AUDIO_BLOCK_SAMPLES + i] / 32768.0f;
		}
		pEffectsProcessor->processFilterAndSends(fx_L, fx_R, AUDIO_BLOCK_SAMPLES, AUDIO_SAMPLE_RATE);
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		{
			m_input_buffer[i] = (s32)(fx_L[i] * 32768.0f);
			m_input_buffer[AUDIO_BLOCK_SAMPLES + i] = (s32)(fx_R[i] * 32768.0f);
		}
	}

	// ALWAYS-ON continuous record buffer (continuousBuffer.h): store the WET
	// block — AFTER live-pitch + effects have mutated m_input_buffer — so loops
	// record exactly what the musician hears. This is the single staging area
	// every looper copies its clip out of, and the source for latency-backdated
	// record start/stop. Publish the engine's current added latency first so the
	// backdate anchor stays sample-true on the processed stream: the pitch
	// engine's read-offset when engaged, 0 when transpose is off (bypassed).
	g_cbExtraLagSamples = (pLivePitchWrapper && pLivePitchWrapper->isEngaged())
	                    ? (u32)pLivePitchWrapper->latencySamples() : 0;
	cbWriteBlock(m_input_buffer);

	// Link grid only governs the quant once a LOCAL loop exists. On a clear bank
	// the FIRST recording defines the grid itself (see _startEndingRecording) and
	// must stop exactly at the backdated press, NOT snap to a Link-derived grid —
	// otherwise the first take records past the press to the next grid beat
	// ("plays the beat after the button press"). Gate the Link-sets-master block
	// on having at least one recorded clip so a synced peer cannot impose a grid
	// before the musician has laid down their own first loop.
	bool anyRecorded = false;
	for (int i = 0; i < LOOPER_NUM_TRACKS; i++)
		if (getTrack(i)->getNumRecordedClips() > 0) { anyRecorded = true; break; }

	if (lp.linkSynced && anyRecorded)
	{
		float bpm = lp.linkBPM;
		if (bpm > 0)
		{
			u32 raw = (u32)((INTEGRAL_BLOCKS_PER_SECOND * 60.0f * 16.0f) / bpm + 0.5f);
			u32 blocks = ((raw + 4) / 8) * 8;
			if (blocks != m_masterLoopBlocks)
			{
				u32 oldBlocks = m_masterLoopBlocks;
				m_masterLoopBlocks = blocks;
				LOOPER_LOG("link quantum: bpm=%.1f masterLoopBlocks=%u (was %u)", bpm, blocks, oldBlocks);

				if (oldBlocks > 0 && blocks > 0)
				{
					float tempoRatio = (float)oldBlocks / (float)blocks;
					for (int i = 0; i < LOOPER_NUM_TRACKS; i++)
					{
						loopTrack *pTrack = getTrack(i);
						for (int j = 0; j < LOOPER_NUM_LAYERS; j++)
						{
							loopClip *pClip = pTrack->getClip(j);
							if (pClip && (pClip->getClipState() == CS_PLAYING ||
										   pClip->getClipState() == CS_LOOPING))
							{
								pClip->setTempoRatio(tempoRatio);
							}
						}
					}
				}
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

	// SHIFT-held monitor mode: ramp the loop-output contribution toward 0 so
	// only the live (transposed + effected) input is heard for temporary
	// record/effect; release ramps back to 1. The clips keep advancing
	// (m_play_block tracks masterPhase above, untouched here), so release lands
	// every loop exactly on the phrase grid — phase-seamless. The ramp is
	// per-sample-interpolated across the block (block-constant endpoints) so the
	// gate transition is click-free. ~MONITOR_GATE_BLOCKS blocks ~= 21ms full
	// travel. The gate touches ONLY the output sum, never the record source
	// (cbWriteBlock already ran above) or the clip phase, so recording during
	// monitor captures the live input and loops resume in phase.
	m_monitorActive = lp.monitorMode;
	const float MONITOR_GATE_STEP = 1.0f / 16.0f;   // full 1<->0 travel in 16 blocks
	float gateStart = m_loopOutputGain;
	float gateTarget = lp.monitorMode ? 0.0f : 1.0f;
	float gateEnd = gateStart;
	if (gateEnd < gateTarget) { gateEnd += MONITOR_GATE_STEP; if (gateEnd > gateTarget) gateEnd = gateTarget; }
	else if (gateEnd > gateTarget) { gateEnd -= MONITOR_GATE_STEP; if (gateEnd < gateTarget) gateEnd = gateTarget; }
	m_loopOutputGain = gateEnd;
	const float gateSampStep = (AUDIO_BLOCK_SAMPLES > 0)
	                         ? (gateEnd - gateStart) / (float)AUDIO_BLOCK_SAMPLES : 0.0f;

	s32 *iip = m_input_buffer;
	s32 *iop = m_output_buffer;

	for (u16 channel=0; channel<LOOPER_NUM_CHANNELS; channel++)
	{
		s16 *op = out[channel]->data;
		float gate = gateStart;

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

			// Apply the monitor gate to the loop contribution only. Input
			// (ival32) always passes at full level. gate ramps per-sample
			// across the block from gateStart to gateEnd (click-free).
			oval32 = (s32)((float)oval32 * gate);
			gate += gateSampStep;

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
	}

	for (u16 channel=0; channel<LOOPER_NUM_CHANNELS; channel++)
	{
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

        // Beat grid: m_masterLoopBlocks encodes 16 beats (see the link-quantum
        // calc, raw = INTEGRAL_BLOCKS_PER_SECOND*60*16/bpm), so one beat =
        // M/16. A pending record latches at the nearest BEAT grid point, NOT
        // only at the full-phrase downbeat (phase % M == 0) — waiting for the
        // whole 16-beat phrase made a press near a beat start a whole phrase
        // later, on the wrong subdivision (the "offbeat / one forward" bug).
        // gridStep clamped >=1; falls back to the phrase boundary when M < 16.
        u32 gridStep = (m_masterLoopBlocks >= 16) ? (m_masterLoopBlocks / 16) : m_masterLoopBlocks;
        bool at_beat_grid = (m_masterLoopBlocks > 0) && gridStep > 0 &&
                            (m_masterPhase % gridStep == 0);
        bool track_latch;
        if (m_track_pending[i] == LOOP_COMMAND_RECORD && m_masterLoopBlocks > 0)
            track_latch = at_beat_grid;
        else
            track_latch =
                !pTrack->getNumRunningClips() ||
                (clip0_state == CS_PLAYING || clip0_state == CS_LOOPING) && !pClip0->getPlayBlockNum() ||
                (clip0_state == CS_RECORDING_MAIN);

        if (track_latch)
        {
            u16 cmd = m_track_pending[i];
            m_track_pending[i] = LOOP_COMMAND_NONE;
            // Witness the grid alignment of a record latch for the :4445 TIME
            // verb (gridStep + the phase the latch fired at) so the
            // offbeat-vs-beat behavior is observable on hardware without audio
            // capture. Only meaningful for a RECORD latch on a grid.
            if (cmd == LOOP_COMMAND_RECORD && m_masterLoopBlocks > 0)
            {
                g_lastGridStep   = (m_masterLoopBlocks >= 16) ? (m_masterLoopBlocks / 16) : m_masterLoopBlocks;
                g_lastLatchPhase = m_masterPhase;
            }
            LOOPER_LOG("TRACK(%d) latching %s", i, getLoopCommandName(cmd));
            pTrack->updateState(cmd);
        }
    }

}

int loopMachine::getWrapperDebugStates(publicLoopMachine::ClipWrapperDebug *out, int maxCount)
{
    int count = 0;
    for (int i = 0; i < LOOPER_NUM_TRACKS && count < maxCount; i++)
    {
        loopTrack *pTrack = getTrack(i);
        for (int j = 0; j < LOOPER_NUM_LAYERS && count < maxCount; j++)
        {
            loopClip *pClip = pTrack->getClip(j);
            if (pClip)
            {
                out[count].track_num = i;
                out[count].clip_num = j;
                out[count].state = pClip->getWrapperDebugState();
                count++;
            }
        }
    }
    return count;
}

// Capture-free clip-FSM telemetry for the :4445 CLIP verb. Fills clip-0 state of
// track `t` so the first-loop seam/wrap is witnessable live (no audio capture):
// state, play_block, record_block, num_blocks, max_blocks. Read on Core 2.
extern "C" void loopClipTelemetry(int t, int *state, unsigned *play, unsigned *rec,
                                  unsigned *num, unsigned *maxb, unsigned *running)
{
    if (!pTheLoopMachine || t < 0 || t >= LOOPER_NUM_TRACKS) { *state=-1; return; }
    loopTrack *pTrack = ((loopMachine*)pTheLoopMachine)->getTrack(t);
    loopClip *pClip = pTrack ? pTrack->getClip(0) : 0;
    *running = pTrack ? (unsigned)pTrack->getNumRunningClips() : 0;
    if (!pClip) { *state=-1; return; }
    *state = (int)pClip->getClipState();
    *play  = pClip->getPlayBlockNum();
    *rec   = pClip->getRecordBlockNum();
    *num   = pClip->getNumBlocks();
    *maxb  = pClip->getMaxBlocks();
}

// Capture-free SHIFT-hold monitor telemetry for the :4445 TIME verb. monitor=1
// while the loop-output gate is ramping toward / held at 0; loopGate100 is the
// current loop-output gain * 100 (100 = loops at full level, 0 = fully muted).
extern "C" void loopMonitorTelemetry(int *monitor, int *loopGate100)
{
    if (!pTheLoopMachine) { *monitor = 0; *loopGate100 = 100; return; }
    *monitor = pTheLoopMachine->m_monitorActive ? 1 : 0;
    *loopGate100 = (int)(pTheLoopMachine->m_loopOutputGain * 100.0f);
}
