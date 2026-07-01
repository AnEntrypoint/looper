#include "Looper.h"
#include "continuousBuffer.h"
#include <circle/logger.h>
#include <circle/util.h>

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
        case CS_FINISHING:
            rp = getBlock(m_record_block);
            break;
        case CS_RECORDING_MAIN:
            // Record only, no playback during main recording phase to avoid read-write race
            rp = getBlock(m_record_block);
            break;
        case CS_RECORDING_TAIL:
            // Recording with playback feedback: write new input, play back completed blocks only
            rp = getBlock(m_record_block);
            if (m_record_block > 0)
                pp_main = getBlock(m_play_block);
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

    // VARISPEED Link-sync read: when the external tempo differs from this loop's
    // native tempo, m_playRate != 1 (set absolutely via setMasterBlocks). Read the clip
    // fractionally at m_playRate -> the loop is resampled, so a halved external tempo
    // plays the loop at half speed AND half pitch (the simplest possible time-stretch).
    // Replaces the block-granular phase-locked read below; the normal read/advance are
    // skipped (pp_main/pp_fade nulled, advance gated on !varispeed). m_playPos is
    // advanced unconditionally in the advance section so it tracks like the phase-lock
    // (pause keeps the head moving). At rate==1 (no external tempo change) this branch
    // is OFF and playback is byte-identical to before.
    //
    // effectiveRate layers the momentary global speed-scrub (notes 70/71,
    // g_globalSpeedMul: 1.0 off, 2.0 double, 0.5 half) ON TOP of m_playRate --
    // it is a separate multiplier, never written into m_playRate, so Link's
    // tempo/quantization state is untouched and the effect reverts exactly on
    // release. m_playPos is never reset when the multiplier engages/releases,
    // only its per-block advance changes, so the playhead position itself
    // (which sample plays "now") is never jumped by holding/releasing 70/71 --
    // it simply advances faster or slower than it otherwise would have.
    float effectiveRate = m_playRate * g_globalSpeedMul;
    bool varispeed = (effectiveRate != 1.0f) && (m_state == CS_PLAYING) && m_num_blocks > 0;
    if (varispeed) { pp_main = 0; pp_fade = 0; }

    bool fade_in = (pp_main && use_play_block < CROSSFADE_BLOCKS);

    s16 tmp_L[AUDIO_BLOCK_SAMPLES] = {0};
    s16 tmp_R[AUDIO_BLOCK_SAMPLES] = {0};

    // COPY-FROM-ROLLING: the clip records by copying from the always-on 3-min
    // continuous buffer (continuousBuffer.h), NOT the live input. The source
    // block is the backdated rec start plus how far we are into the clip, so
    // clip block 0 == the press instant (latency already compensated at start).
    // ip (live input) is now ignored for recording; it stays consumed by the
    // playback/mix below. Both clip dest and rolling source are interleaved
    // s16 [L,R]*AUDIO_BLOCK_SAMPLES -> one memcpy per block.
    if (rp)
    {
        const s16 *src = cbBlockPtr(m_recStartBlock + m_record_block);
        memcpy(rp, src, AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS * sizeof(s16));
    }
    (void)ip;

    double i_fade = 1.0;
    double o_fade = 1.0;
    if (fade_in)
        i_fade = ((double)use_play_block) * FADE_BLOCK_INCREMENT;
    if (pp_fade)
        o_fade = ((double)(CROSSFADE_BLOCKS - m_crossfade_offset)) * FADE_BLOCK_INCREMENT;

    // PAUSE = MUTE (click-free). m_paused gates ONLY the output: m_pauseGain
    // ramps toward 0 when paused and 1 when not, block-constant endpoints
    // interpolated per-sample (1/16 per block ~= 21ms travel) so engage/release
    // and rapid mute/unmute never click. The play-head advance below is OUTSIDE
    // this guard and runs unconditionally, so position is phase-locked to the
    // master regardless of pause state (resume is position-identical, instant).
    const double PAUSE_GAIN_STEP = 1.0 / 16.0;
    double pgStart  = m_pauseGain;
    double pgTarget = m_paused ? 0.0 : 1.0;
    double pgEnd    = pgStart;
    if (pgEnd < pgTarget) { pgEnd += PAUSE_GAIN_STEP; if (pgEnd > pgTarget) pgEnd = pgTarget; }
    else if (pgEnd > pgTarget) { pgEnd -= PAUSE_GAIN_STEP; if (pgEnd < pgTarget) pgEnd = pgTarget; }
    m_pauseGain = pgEnd;
    double pgStep = (pgEnd - pgStart) / (double)AUDIO_BLOCK_SAMPLES;
    double pg = pgStart;

    if (!m_mute && varispeed)
    {
        // Fractional resample read at m_playRate (linear interp). localPos is the
        // block-start copy; m_playPos itself advances once per block in the advance
        // section so it tracks even when muted.
        u32 clipSamples = m_num_blocks * AUDIO_BLOCK_SAMPLES;
        double localPos = m_playPos;
        if (localPos >= (double)clipSamples) localPos = 0.0;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            u32 i0 = (u32)localPos;
            if (i0 >= clipSamples) i0 = 0;
            double fr = localPos - (double)i0;
            u32 i1 = i0 + 1; if (i1 >= clipSamples) i1 = 0;
            s16 l0 = m_buffer[i0 * LOOPER_NUM_CHANNELS],     l1 = m_buffer[i1 * LOOPER_NUM_CHANNELS];
            s16 r0 = m_buffer[i0 * LOOPER_NUM_CHANNELS + 1], r1 = m_buffer[i1 * LOOPER_NUM_CHANNELS + 1];
            double L = ((double)l0 + (double)(l1 - l0) * fr) * m_volume * pg;
            double R = ((double)r0 + (double)(r1 - r0) * fr) * m_volume * pg;
            tmp_L[i] += (s16)(L + (L >= 0 ? 0.5 : -0.5));
            tmp_R[i] += (s16)(R + (R >= 0 ? 0.5 : -0.5));
            localPos += (double)effectiveRate;
            if (localPos >= (double)clipSamples) localPos -= (double)clipSamples;
            pg += pgStep;
        }
    }
    else if (!m_mute)
    {
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            // Read L and R from playback pointers (interleaved)
            if (pp_main)
            {
                double val = *pp_main++ * m_volume * pg;
                if (fade_in) { val *= i_fade; i_fade += FADE_SAMPLE_INCREMENT; }
                tmp_L[i] += (s16)(val + (val >= 0 ? 0.5 : -0.5));
            }
            if (pp_main)
            {
                double val = *pp_main++ * m_volume * pg;
                tmp_R[i] += (s16)(val + (val >= 0 ? 0.5 : -0.5));
            }

            if (pp_fade)
            {
                double val = *pp_fade++ * m_volume * o_fade * pg;
                tmp_L[i] += (s16)(val + (val >= 0 ? 0.5 : -0.5));
            }
            if (pp_fade)
            {
                double val = *pp_fade++ * m_volume * o_fade * pg;
                tmp_R[i] += (s16)(val + (val >= 0 ? 0.5 : -0.5));
                o_fade -= FADE_SAMPLE_INCREMENT;
            }
            pg += pgStep;
        }
    }

    // Per-clip peak level for grid VU LEDs. Use the abs-max of the L+R
    // tmp buffer (= this clip's own contribution this block) so each pad's
    // LED reflects only its own audio, not the whole track sum.
    u32 clipPeak = 0;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        s16 l = tmp_L[i]; if (l < 0) l = -l;
        s16 r = tmp_R[i]; if (r < 0) r = -r;
        if ((u32)l > clipPeak) clipPeak = (u32)l;
        if ((u32)r > clipPeak) clipPeak = (u32)r;
    }
    if (clipPeak > m_clipPeakLevel) m_clipPeakLevel = clipPeak;

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        op[i*LOOPER_NUM_CHANNELS] += (s32)tmp_L[i];
        op[i*LOOPER_NUM_CHANNELS+1] += (s32)tmp_R[i];
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
            bool play = m_quantizeWillPlay;
            m_quantizeTarget = 0;
            m_quantizeWillPlay = false;
            _startEndingRecording(trim, play);
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

        // CS_RECORDING_TAIL plays back for immediate monitoring while the
        // crossfade tail is still being recorded. Playback here must NEVER drive
        // a state transition: the TAIL ends ONLY via _finishRecording (record
        // block-count path) at m_record_block>=m_max_blocks. Previously the
        // advance below ran during TAIL and, for the first loop (L>=masterLen),
        // hit _startCrossFade() at play_block==num_blocks — flipping to CS_LOOPING
        // mid-tail so _finishRecording never fired, double-counting incDecRunning
        // and entering the loop from an ambiguous seam (the first loop "didn't
        // repeat the exact region / played from a funny place"). During TAIL we
        // just advance+wrap the monitor head with no state change.
        if (m_state == CS_RECORDING_TAIL)
        {
            m_play_block++;
            if (m_play_block >= m_num_blocks) m_play_block = 0;
        }
        // Sub-phrase loops (L < M) AND the first loop (L == M) are phase-locked to
        // the master: play_block = (masterPhase - recordStartPhaseOffset) % L,
        // recomputed from masterPhase every block. Block i was recorded at phase
        // offset+i, so (phase-offset)%L == i is the TRUE block-to-phase mapping —
        // it advances by 1 per block and wraps at L exactly like a self-advance,
        // but because it is re-derivable from masterPhase, PAUSE/RESUME lands on the
        // SAME grid position a never-paused clip would (resume must not change phrase
        // sync). The old code self-advanced L==M, so _startPlaying's phase formula
        // and the self-advancing head DIVERGED on resume when recordStartPhaseOffset
        // != 0 (first loop recorded mid-phrase) => the clip resumed OFFBEAT by up to
        // ~L blocks. Witnessed by scripts/test-resume-phase.cpp. Only L > M (and the
        // no-master case) still self-advances below. The wrap->LOOPING crossfade is
        // gated on CS_PLAYING (only a settled, playing loop wraps).
        else if (masterLen > 0 && m_num_blocks <= masterLen)
        {
            u32 next = ((pTheLoopMachine->m_masterPhase - m_recordStartPhaseOffset) % m_num_blocks + m_num_blocks) % m_num_blocks;
            bool wrapped = (next == 0) && (m_play_block > 0);
            if (wrapped && m_state == CS_PLAYING)
            {
                m_state = CS_LOOPING;
                m_crossfade_start = m_num_blocks;
                m_crossfade_offset = 0;
                m_pLoopTrack->incDecRunning(1);
            }
            m_play_block = next;
        }
        // First loop (L==masterLen) and phrase-or-longer loops (L>=M, and the
        // no-master case) self-advance monotonically from their block-0 start and
        // wrap via _startCrossFade — but ONLY when actually PLAYING, never during
        // TAIL (handled above).
        else
        {
            m_play_block++;
            if (m_play_block == m_num_blocks && m_state == CS_PLAYING) _startCrossFade();
            else if (m_play_block >= m_num_blocks) m_play_block = 0;

            // PHRASE-BOUNDARY RE-ANCHOR for L>M loops: at every phrase downbeat
            // (masterPhase % M == 0) while CS_PLAYING (not mid-crossfade), snap
            // m_play_block to the exact grid position derived from masterPhase.
            // Self-advance accumulates no per-step error, but two L>M loops started
            // at different times stay permanently out of phase with each other.
            // The formula is identical to the sub-phrase lock so the snap is always
            // at an expected grid position and produces no audio discontinuity.
            // CS_PLAYING gate avoids corrupting the crossfade block index (CS_LOOPING).
            if (masterLen > 0 && m_num_blocks > 0 && m_state == CS_PLAYING &&
                (pTheLoopMachine->m_masterPhase % masterLen) == 0)
            {
                u32 expected = (u32)(pTheLoopMachine->m_masterPhase - m_recordStartPhaseOffset) % m_num_blocks;
                m_play_block = expected;
            }
        }
        // Keep the varispeed head aligned with the phase-locked block head while
        // rate==1, so when an external tempo change first engages varispeed it picks
        // up from the exact current position.
        m_playPos = (double)m_play_block * (double)AUDIO_BLOCK_SAMPLES;
    }

    // VARISPEED advance (runs whether muted or not, like the head above): advance the
    // fractional position one block's worth at m_playRate and wrap at the loop end.
    // After each natural loop wrap, quantise m_playPos to a block boundary so
    // floating-point accumulation errors (AUDIO_BLOCK_SAMPLES*m_playRate iterated
    // thousands of times) never compound into a sub-sample drift that shifts all
    // subsequent reads.
    if (varispeed)
    {
        u32 clipSamples = m_num_blocks * AUDIO_BLOCK_SAMPLES;
        if (clipSamples > 0)
        {
            m_playPos += (double)AUDIO_BLOCK_SAMPLES * (double)m_playRate;
            bool wrapped = false;
            while (m_playPos >= (double)clipSamples) { m_playPos -= (double)clipSamples; wrapped = true; }
            if (wrapped)
            {
                // Snap to the nearest block boundary so float drift resets each loop.
                u32 blk = ((u32)m_playPos) / AUDIO_BLOCK_SAMPLES;
                if (blk >= m_num_blocks) blk = 0;
                m_playPos = (double)blk * (double)AUDIO_BLOCK_SAMPLES;
            }
            if (m_playPos < 0.0) m_playPos = 0.0;
            m_play_block = ((u32)m_playPos) / AUDIO_BLOCK_SAMPLES;
            if (m_play_block >= m_num_blocks) m_play_block = 0;
        }
    }
}
