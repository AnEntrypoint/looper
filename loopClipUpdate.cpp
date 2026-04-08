#include "Looper.h"
#include <circle/logger.h>

#define log_name "lclip"

#define FADE_BLOCK_INCREMENT (1.0/((double)CROSSFADE_BLOCKS))
#define FADE_SAMPLE_INCREMENT (FADE_BLOCK_INCREMENT/((double)AUDIO_BLOCK_SAMPLES))

void loopClip::update(s32 *ip, s32 *op)
{
    s16 *rp = 0;
    s16 *pp_main = 0;
    s16 *pp_fade = 0;
    uint32_t use_play_block = m_play_block;

    switch (m_state)
    {
        case CS_RECORDING:
        case CS_RECORDING_MAIN:
        case CS_RECORDING_TAIL:
        case CS_FINISHING:
            rp = getBlock(m_record_block);
            break;
        case CS_PLAYING:
            if (m_mark_point_active)
            {
                if (m_play_block == 0) m_play_block = m_mark_point;
                use_play_block = m_play_block - m_mark_point;
            }
            pp_main = getBlock(m_play_block);
            break;
        case CS_LOOPING:
            if (m_mark_point_active)
            {
                if (m_play_block == 0) m_play_block = m_mark_point;
                use_play_block = m_play_block - m_mark_point;
            }
            pp_main = getBlock(m_play_block);
            pp_fade = getBlock(m_crossfade_start + m_crossfade_offset);
            break;
        case CS_STOPPING:
            pp_fade = getBlock(m_crossfade_start + m_crossfade_offset);
            break;
        default:
            break;
    }

    bool fade_in = (pp_main && use_play_block < CROSSFADE_BLOCKS);

    for (int channel = 0; channel < LOOPER_NUM_CHANNELS; channel++)
    {
        double i_fade = 1.0;
        double o_fade = 1.0;
        if (fade_in)
            i_fade = ((double)use_play_block) * FADE_BLOCK_INCREMENT;
        if (pp_fade)
            o_fade = ((double)(CROSSFADE_BLOCKS - m_crossfade_offset)) * FADE_BLOCK_INCREMENT;

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            if (rp) *rp++ = *ip++;

            if (!m_mute)
            {
                if (pp_main)
                {
                    double val = *pp_main++ * m_volume;
                    if (fade_in) { val *= i_fade; i_fade += FADE_SAMPLE_INCREMENT; }
                    *op += (s32)val;
                }
                if (pp_fade)
                {
                    double val = *pp_fade++ * m_volume * o_fade;
                    *op += (s32)val;
                    o_fade -= FADE_SAMPLE_INCREMENT;
                }
            }
            op++;
        }
    }

    if (rp)
    {
        m_record_block++;
        if (m_state == CS_RECORDING && m_record_block >= CROSSFADE_BLOCKS)
            m_state = CS_RECORDING_MAIN;

        if ((m_state == CS_RECORDING_MAIN) &&
            m_quantizeTarget > 0 &&
            m_record_block >= m_quantizeTarget)
        {
            u32 trim = m_quantizeTarget;
            m_quantizeTarget = 0;
            _startEndingRecording(trim, true);
        }
        else if ((m_state == CS_RECORDING_TAIL || m_state == CS_FINISHING) &&
            m_record_block >= m_max_blocks)
        {
            _finishRecording();
        }
    }

    if (pp_fade)
    {
        m_crossfade_offset++;
        if (m_crossfade_offset == CROSSFADE_BLOCKS)
        {
            if (m_state == CS_LOOPING)
            {
                m_state = CS_PLAYING;
                m_crossfade_start = 0;
                m_crossfade_offset = 0;
                m_pLoopTrack->incDecRunning(-1);
            }
            else
            {
                _endFadeOut();
            }
        }
    }

    if (pp_main)
    {
        u32 masterLen = pTheLoopMachine->m_masterLoopBlocks;
        if (masterLen > 0 && m_state != CS_LOOPING)
        {
            u32 next = pTheLoopMachine->m_masterPhase % m_num_blocks;
            bool wrapped = (next == 0) && (m_play_block > 0);
            m_play_block = next;
            if (wrapped) _startCrossFade();
        }
        else
        {
            m_play_block++;
            if (m_play_block == m_num_blocks) _startCrossFade();
        }
    }
}
