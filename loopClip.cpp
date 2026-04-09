#include "Looper.h"
#include "abletonLink.h"
#include <circle/logger.h>

#define log_name "lclip"

loopClip::loopClip(u16 clip_num, loopTrack *pTrack) :
    publicClip(pTrack->getTrackNum(), clip_num)
{
    m_pLoopTrack = pTrack;
    init();
}

loopClip::~loopClip()
{
    init();
    m_pLoopTrack = 0;
}

void loopClip::init()
{
    publicClip::init();
    m_buffer = 0;
    m_mark_point = -1;
    m_mark_point_active = false;
}

void loopClip::clearMarkPoint()
{
    LOOPER_LOG("clip(%d:%d) clearMarkPoint", m_track_num, m_clip_num);
    m_mark_point = -1;
    m_mark_point_active = false;
}

void loopClip::halveLength()
{
    if (m_num_blocks <= CROSSFADE_BLOCKS * 2) return;
    if (m_origNumBlocks == 0) m_origNumBlocks = m_num_blocks;
    m_num_blocks = m_num_blocks / 2;
}

void loopClip::doubleLength()
{
    u32 doubled = m_num_blocks * 2;
    u32 needed = doubled + CROSSFADE_BLOCKS;
    if (needed > m_max_blocks) return;
    s16 *src = getBlock(0);
    s16 *dst = getBlock(m_num_blocks);
    u32 samplesToCopy = m_num_blocks * AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS;
    memcpy(dst, src, samplesToCopy * sizeof(s16));
    pTheLoopBuffer->commitBlocks(needed * LOOPER_NUM_CHANNELS);
    m_origNumBlocks = doubled;
    m_num_blocks = doubled;
    m_max_blocks = needed;
    if (!linkIsSynced() && m_clip_num == 0 && m_num_blocks > pTheLoopMachine->m_masterLoopBlocks)
    {
        pTheLoopMachine->m_masterLoopBlocks = m_num_blocks;
        pTheLoopMachine->m_masterPhase = pTheLoopMachine->m_masterPhase % m_num_blocks;
    }
}

void loopClip::setMarkPoint()
{
    LOOPER_LOG("clip(%d:%d) setMarkPoint=%d", m_track_num, m_clip_num, m_play_block);
    m_mark_point = m_play_block;
}

void loopClip::stopImmediate()
{
    LOOPER_LOG("clip(%d,%d)::stopImmediate state=%d", m_track_num, m_clip_num, (int)m_state);
    switch (m_state)
    {
        case CS_RECORDING:
        case CS_RECORDING_MAIN:
        case CS_RECORDING_TAIL:
        case CS_FINISHING:
            m_pLoopTrack->incDecRunning(-1);
            m_pLoopTrack->incDecNumUsedClips(-1);
            if (m_state == CS_RECORDING_TAIL || m_state == CS_FINISHING)
                m_pLoopTrack->incDecNumRecordedClips(-1);
            init();
            break;
        case CS_PLAYING:
            m_pLoopTrack->incDecRunning(-1);
            m_play_block = 0;
            m_state = CS_RECORDED;
            break;
        case CS_LOOPING:
            m_pLoopTrack->incDecRunning(-2);
            m_play_block = 0;
            m_crossfade_start = 0;
            m_crossfade_offset = 0;
            m_state = CS_RECORDED;
            break;
        case CS_STOPPING:
            m_pLoopTrack->incDecRunning(-1);
            m_play_block = 0;
            m_crossfade_start = 0;
            m_crossfade_offset = 0;
            m_state = CS_RECORDED;
            break;
        default:
            break;
    }
}

void loopClip::_startRecording()
{
    LOOPER_LOG("clip(%d,%d)::startRecording()", m_track_num, m_clip_num);
    m_play_block = 0;
    m_record_block = 0;
    m_crossfade_start = 0;
    m_crossfade_offset = 0;
    m_num_blocks = 0;
    m_max_blocks = (pTheLoopBuffer->getFreeBlocks() / LOOPER_NUM_CHANNELS) - CROSSFADE_BLOCKS;
    m_buffer = pTheLoopBuffer->getBuffer();
    m_recordStartPhaseOffset = pTheLoopMachine->m_masterPhase;
    CLogger::Get()->Write("lclip", LogNotice, "startRecording: startPhase=%u masterLen=%u", m_recordStartPhaseOffset, pTheLoopMachine->m_masterLoopBlocks);
    m_state = CS_RECORDING;
    m_pLoopTrack->incDecNumUsedClips(1);
    m_pLoopTrack->incDecRunning(1);
}

void loopClip::_startEndingRecording(u32 trimToBlocks, bool willPlay)
{
    LOOPER_LOG("clip(%d,%d)::startEndingRecording(trim=%d,play=%d)", m_track_num, m_clip_num, trimToBlocks, willPlay);
    m_num_blocks = (trimToBlocks > 0) ? trimToBlocks : m_record_block;
    m_max_blocks = m_num_blocks + CROSSFADE_BLOCKS;
    CLogger::Get()->Write("lclip", LogNotice, "endRecording: recorded=%u target=%u numBlocks=%u", m_record_block, trimToBlocks, m_num_blocks);
    pTheLoopBuffer->commitBlocks(m_max_blocks * LOOPER_NUM_CHANNELS);
    if (!linkIsSynced() && m_clip_num == 0 && m_num_blocks > pTheLoopMachine->m_masterLoopBlocks)
    {
        pTheLoopMachine->m_masterLoopBlocks = m_num_blocks;
        pTheLoopMachine->m_masterPhase = pTheLoopMachine->m_masterPhase % m_num_blocks;
    }
    m_state = willPlay ? CS_RECORDING_TAIL : CS_FINISHING;
    m_pLoopTrack->incDecNumRecordedClips(1);
}

void loopClip::_finishRecording()
{
    LOOPER_LOG("clip(%d,%d)::finishRecording() willPlay=%d", m_track_num, m_clip_num, m_state == CS_RECORDING_TAIL);
    bool willPlay = (m_state == CS_RECORDING_TAIL);
    m_state = CS_RECORDED;
    m_pLoopTrack->incDecRunning(-1);
    if (willPlay)
        _startPlaying();
}

void loopClip::_startPlaying()
{
    u32 masterLen = pTheLoopMachine->m_masterLoopBlocks;
    if (masterLen > 0 && m_num_blocks > 0)
        m_play_block = (pTheLoopMachine->m_masterPhase - m_recordStartPhaseOffset + 2 * m_num_blocks) % m_num_blocks;
    else
        m_play_block = 0;
    CLogger::Get()->Write("lclip", LogNotice, "startPlaying: play_block=%u startPhase=%u masterPhase=%u numBlocks=%u", m_play_block, m_recordStartPhaseOffset, pTheLoopMachine->m_masterPhase, m_num_blocks);
    LOOPER_LOG("clip(%d,%d)::startPlaying(play_block=%d offset=%d)", m_track_num, m_clip_num, m_play_block, m_recordStartPhaseOffset);
    m_crossfade_start = 0;
    m_crossfade_offset = 0;
    m_state = CS_PLAYING;
    m_pLoopTrack->incDecRunning(1);
}
void loopClip::_startCrossFade()
{
    LOOPER_LOG("clip(%d,%d)::startCrossFade", m_track_num, m_clip_num);
    m_state = CS_LOOPING;
    m_crossfade_start = m_play_block;
    m_crossfade_offset = 0;
    m_pLoopTrack->incDecRunning(1);
}

void loopClip::_startFadeOut()
{
    LOOPER_LOG("clip(%d,%d)::startFadeOut", m_track_num, m_clip_num);
    m_crossfade_start = m_play_block ? m_play_block : 0;
    m_state = CS_STOPPING;
    m_crossfade_offset = 0;
    m_play_block = 0;
}

void loopClip::_endFadeOut()
{
    LOOPER_LOG("clip(%d,%d)::endFadeOut", m_track_num, m_clip_num);
    m_state = CS_RECORDED;
    m_crossfade_start = 0;
    m_crossfade_offset = 0;
    m_pLoopTrack->incDecRunning(-1);
}
