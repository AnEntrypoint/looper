#include "Looper.h"
#include "continuousBuffer.h"

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

// Backdated recorded length (blocks) for the FIRST loop (no master grid yet):
// length = backdated stop block - backdated start block, so the loop duration
// equals the press-to-press interval exactly (both ends latency-compensated),
// not the process-to-process interval. Falls back to m_record_block if the
// backdated stop would be <= start (degenerate / no timestamp).
u32 loopClip::_backdatedRecordLength()
{
    // The loop must END exactly at the (backdated) stop press. Clip block 0 is
    // already the backdated START (m_recStartBlock = cbBackdatedBlock at start),
    // so the length is the absolute backdated-stop block minus the start block.
    //
    // stopBlock = g_cbWriteBlock - backStop, and m_record_block (blocks filled
    // since start) == g_cbWriteBlock - m_recStartBlock, so:
    //   len = stopBlock - m_recStartBlock = m_record_block - backStop
    // i.e. we trim the stop-side action latency (backStop blocks) off the END,
    // which is what stops the loop at the press instead of the (later) process
    // moment. Recording kept filling past the press by backStop blocks; we drop
    // them. Clamp to [1, m_record_block]; fall back to m_record_block only if
    // the backdate is degenerate (stop at/behind start, or no timestamp).
    // The clip is anchored in ABSOLUTE ring coordinates: clip block i ==
    // cbBlockPtr(m_recStartBlock + i), and m_recStartBlock = the backdated START
    // press. The loop must END at the backdated STOP press = stopBlock. So the
    // length in clip coordinates is simply stopBlock - m_recStartBlock — the
    // press-to-press interval, both ends already latency-compensated.
    //
    // Do NOT derive length from m_record_block - backStop: m_record_block counts
    // audio updates SINCE recording began, but recording starts catching up from
    // a BACKDATED (past) m_recStartBlock, so the clip runs a fixed backStart
    // blocks behind the live write head — m_record_block != wrNow - m_recStartBlock.
    // That mismatch left the loop a little long at the stop (start on time, stop
    // late). Using absolute ring coordinates removes both backdates' bookkeeping.
    u32 stopBlock = cbBackdatedBlock(g_pendingPressTicks);     // backdated stop press (abs ring block)
    u32 len = (stopBlock > m_recStartBlock) ? (stopBlock - m_recStartBlock) : 0;

    // Never exceed what has actually been copied into the clip yet.
    if (len > m_record_block) len = m_record_block;

    // Floor at CROSSFADE_BLOCKS*2 so the loop is long enough for the seam
    // crossfade machinery (the M>0 quantize candidates apply the same floor; the
    // first loop, M==0, must too). Never exceed what was captured.
    u32 floorLen = (u32)(CROSSFADE_BLOCKS * 2);
    if (len < floorLen) len = (m_record_block >= floorLen) ? floorLen : m_record_block;
    if (len == 0) len = 1;
    return len;
}

u32 loopClip::_calcQuantizeTarget()
{
    u32 M = pTheLoopMachine->m_masterLoopBlocks;
    if (M == 0) return _backdatedRecordLength();

    u32 rec = m_record_block;
    u32 floorLen = (u32)(CROSSFADE_BLOCKS * 2);

    // 505-like quant (user rule): round to the NEAREST power-of-two grid of M,
    // BUT bias so a SHORT tap rounds DOWN (plays immediately) and a near-grid tap
    // rounds UP (extends to the boundary). The decision per candidate: snap UP to
    // a candidate larger than `rec` ONLY when `rec` is at least HALFWAY to it
    // (rec*2 >= candidate); otherwise the candidate below is closer and we take
    // that. This is exactly "nearest on a log-2 grid" and it guarantees the chosen
    // target for a sub-grid tap is <= rec when rec is below the midpoint — so
    // updateState(PLAY) finishes+plays IMMEDIATELY instead of deferring (the
    // "records then silent, never starts" stall when a short tap snapped up to a
    // much larger division). A tap near/just under a phrase (rec close to M) is
    // >= M/2's midpoint toward M, so it rounds UP to M (505 play-through). A tap
    // over a phrase rounds to the nearest multiple {2M,4M,8M,...} with NO hard cap
    // -- previously capped at 8M, which clipped recordings longer than 8 beats
    // when the first loop was 1 beat.
    //
    // Sub-phrase candidates (M/8..M/2) are evaluated first; then the phrase-multiple
    // sweep walks powers of 2 of M until the next candidate would be > 2*rec
    // (unreachable by the rounding rule). Guard against overflow: stop at 2^30.
    u32 sub[] = { floorLen, M/8, M/4, M/2 };
    u32 best = floorLen;
    for (int i = 0; i < (int)(sizeof sub / sizeof sub[0]); i++)
    {
        u32 c = sub[i];
        if (c < floorLen) continue;
        if (c <= rec) best = c;
        else { if (rec * 2 > best + c) best = c; break; }
    }
    // Walk phrase multiples: M, 2M, 4M, 8M, 16M, ... until the next step is
    // too far away to round up to (c > 2*rec means even the midpoint exceeds rec).
    //
    // The FIRST multiple (c == M) gets a biased round-up instead of the
    // symmetric midpoint rule: once a phrase length is established, a loop
    // 2+ take that's close to M (>= 0.6M) almost always means the performer
    // intended to match the existing phrasing, not to deliberately record a
    // half-length loop. The symmetric rule required rec > 0.75M (since best
    // was M/2 at this point) to round up, so takes trimmed by normal human
    // stop-press timing or backdate/latency compensation (see
    // _backdatedRecordLength) could land in [0.6M, 0.75M) and get silently
    // quantized to M/2 -- a loop that then re-triggers twice per phrase
    // relative to the others, reading as "shorter and drifting" even though
    // each cycle is phase-exact (see loopClipUpdate.cpp phase re-anchor).
    // Sub-divisions (M/2 and below) still use the symmetric rule above, so a
    // genuinely short take (rec < 0.6M) is unaffected.
    for (u32 mult = 1; mult <= (1u << 30) / (M > 0 ? M : 1); mult <<= 1)
    {
        u32 c = M * mult;
        if (c < floorLen) continue;
        if (c <= rec) { best = c; continue; }
        if (mult == 1) { if (rec * 5 > c * 3) best = c; break; }
        if (rec * 2 > best + c) best = c;
        break;   // c > rec and rec didn't reach the midpoint; no larger cand can win
    }
    return best;
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
                m_quantizeWillPlay = false;
            }
        }
        else if (m_state == CS_PLAYING || m_state == CS_LOOPING)
        {
            // PAUSE = MUTE only. The clip stays CS_PLAYING/CS_LOOPING so its
            // play head keeps advancing (phase-locked to masterPhase); we just
            // latch m_paused, which gates the output to silence (click-free
            // ramp in loopClipUpdate.cpp). Position never changes, so resume is
            // position-identical and rapid mute/unmute is instant. This replaces
            // the old _startFadeOut()->CS_RECORDED path that reset the head to 0.
            // getTrackState() reports a paused clip as STOPPED so the pad blinks
            // yellow and the paused-tap gesture resumes it. The clip stays in the
            // running set (no incDecRunning) precisely so it keeps advancing.
            m_paused = true;
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
                m_quantizeWillPlay = true;
            }
        }
        // Resume a paused (muted) clip = just un-mute. No _startPlaying re-anchor:
        // the head never stopped advancing, so it is already at the correct
        // phase-locked position — un-gating the output resumes audio exactly in
        // phrase with zero position change.
        if ((m_state == CS_PLAYING || m_state == CS_LOOPING) && m_paused)
            m_paused = false;
        else if (m_state == CS_RECORDED)
            _startPlaying();
    }
    else if (cur_command == LOOP_COMMAND_RECORD)
    {
        _startRecording();
    }
}
