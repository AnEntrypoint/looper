#include "Looper.h"
#include "abletonLink.h"
#include "continuousBuffer.h"

#define log_name "lclip"

loopClip::loopClip(u16 clip_num, loopTrack *pTrack) :
    publicClip(pTrack->getTrackNum(), clip_num),
    m_wrapper(AUDIO_SAMPLE_RATE, LOOPER_NUM_CHANNELS)
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
    m_playRate     = 1.0f;
    m_playPos      = 0.0;
    m_nativeBlocks = 0;
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
    // Doubling a clip must NOT redefine the master phrase reference. As with the
    // record-finish path, only the first loop on a clear bank owns the grid;
    // growing m_masterLoopBlocks / reslicing m_masterPhase here would shift every
    // playing clip's phase. Set it only if no master exists yet (degenerate:
    // doubling the very first, still-undefined loop).
    if (!linkIsSynced() && pTheLoopMachine->m_masterLoopBlocks == 0)
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
    // A real stop clears the pause-mute latch: stopImmediate resets the head, so
    // there is nothing left to keep muted. (Pause is the per-track STOP_TRACK
    // gesture; this stopImmediate is the distinct shift+STOP/abort full stop.)
    m_paused = false;
    m_pauseGain = 1.0;
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
    // A fresh recording always starts audible — never inherit a prior take's
    // pause-mute latch.
    m_paused = false;
    m_pauseGain = 1.0;
    m_play_block = 0;
    m_record_block = 0;
    m_crossfade_start = 0;
    m_crossfade_offset = 0;
    m_num_blocks = 0;
    m_playRate = 1.0f;   // fresh take is native to the current grid (varispeed resets)
    m_playPos  = 0.0;
    // Clear any pending quantize auto-stop. A fresh recording must NOT inherit a
    // prior take's deferred-quantize target — otherwise the per-block auto-stop
    // (loopClipUpdate.cpp) fires almost immediately (m_record_block crosses the
    // stale target within a few blocks), ending the take a fraction of a beat
    // after the press and making a tight first loop impossible.
    m_quantizeTarget = 0;
    m_quantizeWillPlay = false;
    m_max_blocks = (pTheLoopBuffer->getFreeBlocks() / LOOPER_NUM_CHANNELS) - CROSSFADE_BLOCKS;
    m_buffer = pTheLoopBuffer->getBuffer();

    // Anchor the clip's content-start (m_recStartBlock = ring block of clip block
    // 0) and its phase (m_recordStartPhaseOffset = the masterPhase playback treats
    // as block 0) to the SAME instant. There are two regimes:
    if (pTheLoopMachine->m_masterLoopBlocks > 0)
    {
        // CONSECUTIVE loop (a master grid exists): this _startRecording was fired
        // by the pending-record LATCH in loopMachine::updateState, which triggers
        // EXACTLY at a beat-grid downbeat (m_masterPhase % gridStep == 0). So the
        // true start IS this instant: g_cbWriteBlock is the ring block being
        // written now, and m_masterPhase is a clean beat. Anchor to them directly,
        // with NO backdate and NO re-snap.
        //   - NO backdate: the wait-for-grid latch already absorbed the press->grid
        //     latency; backdating here would use g_pendingPressTicks, which is
        //     STALE at latch time (it is only set in the APC cmd drain when the
        //     pending was SET, never refreshed for the later latch) — backdating a
        //     stale tick moved the content start to the wrong instant.
        //   - NO re-snap: round-to-nearest on the backdated phase could land on a
        //     neighboring (later) beat than the latch downbeat, shifting the loop
        //     late off the first-loop start. The latch IS the grid point already.
        //
        // Content anchor uses cbBackdatedBlock(0), NOT g_cbWriteBlock: g_cbWriteBlock
        // is the NEXT-to-write block (cbWriteBlock writes dst THEN increments), so
        // anchoring there made clip block 0 read the UNWRITTEN/stale ring head =
        // silence -> the consecutive clip recorded nothing and played no audio.
        // cbBackdatedBlock(0) (press_ticks==0 -> fixed-lag only = g_cbWriteBlock -
        // CB_FIXED_LAG_SAMPLES/AUDIO_BLOCK_SAMPLES) lands on WRITTEN history with the
        // same fixed ring/ADC lag the first loop uses — real audio at block 0. It
        // ignores the stale g_pendingPressTicks. Phase stays the latch beat.
        m_recStartBlock          = cbBackdatedBlock(0);
        m_recordStartPhaseOffset = pTheLoopMachine->m_masterPhase;
    }
    else
    {
        // FIRST loop (clear bank, no master): started IMMEDIATELY on the press
        // (no latch wait), so backdate to the press instant — clip block 0 ==
        // the backdated press. It DEFINES the grid; left sample-true, no snap.
        // Phase = masterPhase at that backdated content start.
        m_recStartBlock = cbBackdatedBlock(g_pendingPressTicks);
        u32 backBlocks = g_cbWriteBlock - m_recStartBlock;
        m_recordStartPhaseOffset = pTheLoopMachine->m_masterPhase - backBlocks;
    }
    LOOPER_LOG("startRecording: startPhase=%u masterLen=%u recStartBlock=%u", m_recordStartPhaseOffset, pTheLoopMachine->m_masterLoopBlocks, m_recStartBlock);

    // FIRST loop on a clear bank (no master grid, not Link-synced to peers):
    // mark Link "have started" and anchor the timeline origin at the backdated
    // press instant, so this loop becomes beat 0 and replays/restarts seamlessly.
    // Runs EVEN when synced: the first loop PROPOSES our tempo/phase to the session
    // (Ableton Link lets any device set the group tempo) so the ticker/Live adopt it
    // rather than us only ever following.
    if (pTheLoopMachine->m_masterLoopBlocks == 0)
        linkStart(g_pendingPressTicks);

    m_state = CS_RECORDING;
    m_pLoopTrack->incDecNumUsedClips(1);
    m_pLoopTrack->incDecRunning(1);
}

void loopClip::_startEndingRecording(u32 trimToBlocks, bool willPlay)
{
    LOOPER_LOG("clip(%d,%d)::startEndingRecording(trim=%d,play=%d)", m_track_num, m_clip_num, trimToBlocks, willPlay);
    m_num_blocks = (trimToBlocks > 0) ? trimToBlocks : m_record_block;
    m_max_blocks = m_num_blocks + CROSSFADE_BLOCKS;
    LOOPER_LOG("endRecording: recorded=%u target=%u numBlocks=%u", m_record_block, trimToBlocks, m_num_blocks);
    pTheLoopBuffer->commitBlocks(m_max_blocks * LOOPER_NUM_CHANNELS);

    // The master phrase reference is set EXACTLY ONCE, by the FIRST loop
    // recorded on a clear bank (m_masterLoopBlocks == 0). Every subsequent loop
    // — 2nd, 3rd, longer or shorter — must NEVER grow m_masterLoopBlocks or
    // reslice m_masterPhase: both feed every playing clip's phase
    // (play_block = (masterPhase - recordStartPhaseOffset) % num_blocks), so
    // changing them shifts loops already playing and corrupts their phrasing
    // (the intermittent "a 3rd loop messed up an existing loop" bug — it fired
    // whenever the new loop happened to be longer than the established master).
    // Later loops align TO this grid; they do not redefine it.
    bool wasFirst = false;
    if (pTheLoopMachine->m_masterLoopBlocks == 0)
    {
        // FIRST loop: derive tempo+quant from this loop's length (nearest-120
        // subdivision) and PROPOSE it to the Link session. linkEnd runs EVEN when
        // synced so the looper can set the group tempo (any Link device may) — the
        // ticker/Live then adopt it. Quant SOURCE is this first loop.
        double clip_seconds = (double)m_num_blocks / (double)INTEGRAL_BLOCKS_PER_SECOND;
        linkEnd(clip_seconds);

        if (!linkIsSynced())
        {
            // Not following a session: this loop also DEFINES the local master grid.
            // When synced, loopMachine derives m_masterLoopBlocks from the (now
            // proposed) session tempo instead, so loop and grid stay consistent.
            wasFirst = true;
            pTheLoopMachine->m_masterLoopBlocks = m_num_blocks;
            pTheLoopMachine->m_masterPhase = m_recordStartPhaseOffset % m_num_blocks;
        }
    }

    // NO stop-time phase floor. Playback reads the CLIP BUFFER (getBlock(m_play_block)),
    // which was filled INCREMENTALLY during recording from the beat-snapped
    // m_recStartBlock set in _startRecording — so buffer block 0 == the beat-snapped
    // record-start instant, and m_recordStartPhaseOffset already names that exact
    // instant. Flooring the offset to the L-grid here shifted the PHASE while the
    // already-recorded audio stayed put (m_recStartBlock changes are inert post-record
    // since playback uses the buffer, not the ring) — the second loop then played
    // OFFBEAT by floorBack. The beat-snapped offset is already coherent with every
    // quantized L (all of {M/8..8M} are multiples of the M/16 beat grid, and the
    // offset is a multiple of M/16), so play_block=(masterPhase-offset)%L has phase 0
    // exactly at the recorded downbeat and at every L-boundary. Sync needs
    // offset==buffer-block-0, which the record-start beat-snap gives; no floor.

    m_state = willPlay ? CS_RECORDING_TAIL : CS_FINISHING;
    m_play_block = 0;
    m_pLoopTrack->incDecNumRecordedClips(1);
}

void loopClip::_finishRecording()
{
    LOOPER_LOG("clip(%d,%d)::finishRecording() willPlay=%d", m_track_num, m_clip_num, m_state == CS_RECORDING_TAIL);
    // Lock in the current phrase length as the native reference so setMasterBlocks
    // can always compute the correct absolute varispeed ratio from originals.
    if (m_nativeBlocks == 0)
        m_nativeBlocks = pTheLoopMachine->m_masterLoopBlocks;
    bool willPlay = (m_state == CS_RECORDING_TAIL);
    m_state = CS_RECORDED;
    m_pLoopTrack->incDecRunning(-1);
    if (willPlay)
        // Hand off from TAIL to PLAYING WITHOUT resetting the play head: TAIL has
        // been advancing m_play_block monotonically from 0 while capturing the
        // crossfade tail, so the audio is already mid-loop at a continuous
        // position. Preserving it makes the record->play transition seamless (no
        // backward jump of CROSSFADE blocks); the loop then wraps cleanly at
        // num_blocks via _startCrossFade. A FRESH play (resume from stopped) does
        // re-anchor the start position (preservePlayBlock=false).
        _startPlaying(/*preservePlayBlock=*/true);
}

void loopClip::_startPlaying(bool preservePlayBlock)
{
    u32 masterLen = pTheLoopMachine->m_masterLoopBlocks;
    if (preservePlayBlock)
    {
        // Continue from the TAIL monitor position — already a valid in-loop block
        // in [0, m_num_blocks). Clamp defensively.
        if (m_num_blocks > 0 && m_play_block >= m_num_blocks)
            m_play_block %= m_num_blocks;
    }
    else if (masterLen > 0 && m_num_blocks > 0)
    {
        if (m_num_blocks >= masterLen)
        {
            // Phrase-or-longer loop (L = M, 2M, 4M): ALWAYS resume from its OWN
            // BEGINNING (block 0), beginning on the next phrase downbeat. The
            // free-running masterPhase only tracks the phrase grid (M), so
            // (masterPhase - offset) % L for a long L wanders by a phrase on each
            // restart depending on which phrase you happened to restart in ("3rd
            // loop changed when restarting"). There is no stable absolute L-anchor
            // to resume mid-loop against, and the user rule is explicit: a loop a
            // phrase or longer starts from phrase start. So we restart it at block
            // 0; play continues 0..L-1 across the following phrases. Deterministic
            // every time. (The phase-locked sub-phrase branch is for L < M only.)
            m_play_block = 0;
        }
        else
        {
            // Sub-phrase loop: L divides M, so the free-running masterPhase is
            // coherent modulo L and this is phase-stable across restarts. Starts on
            // a division of the phrase (M/2 halfway, M/4 quarter, ...).
            m_play_block = ((pTheLoopMachine->m_masterPhase - m_recordStartPhaseOffset) % m_num_blocks + m_num_blocks) % m_num_blocks;
        }
    }
    else
        m_play_block = 0;
    LOOPER_LOG("startPlaying: play_block=%u startPhase=%u masterPhase=%u numBlocks=%u", m_play_block, m_recordStartPhaseOffset, pTheLoopMachine->m_masterPhase, m_num_blocks);
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
    // Called from the self-advance path when m_play_block has just reached
    // m_num_blocks (the loop point). The seam crossfades the OUTGOING tail
    // (recorded blocks num_blocks..num_blocks+CROSSFADE) against the NEW loop
    // starting at block 0. So: crossfade_start = the tail start (= current
    // play_block == num_blocks), and the main play head WRAPS TO 0 to restart the
    // exact recorded region. Previously play_block was left at num_blocks, so
    // CS_LOOPING read getBlock(num_blocks) for the main head — the tail, NOT block
    // 0 — making the loop NOT repeat its exact region from the start (the
    // first-loop "funny place" seam). Reset to 0 to wrap cleanly.
    m_crossfade_start = m_play_block;   // = num_blocks: outgoing tail start
    m_play_block = 0;                   // new loop restarts the exact region
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
