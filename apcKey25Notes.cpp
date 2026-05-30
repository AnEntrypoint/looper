#define log_name "apc25"

#include "apcKey25.h"

// Grid mapping (matches /goal): 20 flat loopers + 10 preset slots.
//
//   col 0,1 (10 pads, rows 0..4):  preset slots, index = row*2 + col
//   col 2..5 (20 pads, rows 0..4): loopers,      index = row*4 + (col-2)
//   col 6,7:                       legacy halve/double — kept harmless
//
// Looper press:
//   - Tap toggles state via existing track machinery:
//       empty/idle → record (CLEAR_LAYER cmd starts recording for first clip)
//       recording  → play  (TRACK cmd advances state)
//       playing    → pause (STOP_TRACK)
//       paused     → play  (TRACK)
//   - Long-hold (APC_HOLD_ERASE_MS) → clear the looper.
//
// Preset press:
//   - Tap → restore: every looper in the preset's mask becomes playing,
//     every looper NOT in the mask becomes paused. Empty loopers ignored.
//   - Long-hold → capture: snapshot which loopers are currently running
//     into this preset slot; preset becomes "used" and lights up.

int apcKey25::_looperFromPad(int row, int col) const
{
    if (row < 0 || row >= APC_ROWS) return -1;
    if (col < 2 || col > 5) return -1;
    int idx = row * 4 + (col - 2);
    if (idx < 0 || idx >= LOOPER_NUM_TRACKS) return -1;
    return idx;
}

int apcKey25::_presetFromPad(int row, int col) const
{
    if (row < 0 || row >= APC_ROWS) return -1;
    if (col < 0 || col > 1) return -1;
    int idx = row * 2 + col;
    if (idx < 0 || idx >= LOOPER_NUM_PRESETS) return -1;
    return idx;
}

void apcKey25::_onPadPress(int row, int col)
{
    int looper = _looperFromPad(row, col);
    if (looper >= 0)
    {
        m_looperHeld[looper]            = true;
        m_looperClearTriggered[looper]  = false;
        m_looperHoldStart[looper]       = m_nowMs;

        // Start recording an EMPTY looper on PRESS, not release — this is the
        // timing-critical action ("catch the exact moment record is pressed
        // when the bank is clear"). Release-triggered tap added the press-hold
        // duration as jitter, making first-record starts feel imprecise. An
        // empty pad has nothing to erase, so press-record doesn't conflict with
        // the long-hold-erase gesture (a >1s hold still fires CLEAR_LAYER from
        // the hold poll, which now cancels the just-armed take — desired). All
        // OTHER transitions (finish/pause/play/erase on a pad with content)
        // stay on release where tap-vs-hold disambiguation is needed.
        publicTrack *pTrack = pTheLooper->getPublicTrack(looper);
        int ts = pTrack->getTrackState();
        int recorded = pTrack->getNumRecordedClips();
        if (recorded == 0 && !(ts & (TRACK_STATE_RECORDING | TRACK_STATE_PENDING_RECORD)))
        {
            m_looperRecordedOnPress[looper] = true;   // suppress the release tap
            _queueCmd(ApcCmd::CLEAR_LAYER, looper);   // arm record now
        }
        else if (ts & TRACK_STATE_RECORDING)
        {
            // FINISH recording on PRESS-DOWN, not release — SAME timing-critical
            // reason as the arm: the stop must land on the press instant. Firing
            // on release stamped press_ticks at the release moment and recorded
            // the entire press-hold duration, so the first loop ran a little long
            // past the stop press. Press-down dispatch + backdate ends the loop
            // exactly on the press. A long-hold still erases via the hold poll
            // (ERASE_TRACK clears regardless of state). Suppress the release tap.
            m_looperRecordedOnPress[looper] = true;
            _queueCmd(ApcCmd::TRACK, looper);         // finish + play now
        }
        else
        {
            m_looperRecordedOnPress[looper] = false;
        }
        return;
    }
    int preset = _presetFromPad(row, col);
    if (preset >= 0)
    {
        m_presetHeld[preset]       = true;
        m_presetCaptured[preset]   = false;
        m_presetHoldStart[preset]  = m_nowMs;
        return;
    }
    if (col == 6)
    {
        _queueCmd(ApcCmd::LOOPER, LOOP_COMMAND_HALVE_TRACK_BASE + row);
    }
    else if (col == 7)
    {
        _queueCmd(ApcCmd::LOOPER, LOOP_COMMAND_DOUBLE_TRACK_BASE + row);
    }
}

void apcKey25::_onPadRelease(int row, int col)
{
    int looper = _looperFromPad(row, col);
    if (looper >= 0)
    {
        // Already armed record on press for an empty pad — don't double-fire.
        if (m_looperRecordedOnPress[looper])
        {
            m_looperRecordedOnPress[looper] = false;
            m_looperHeld[looper] = false;
            return;
        }
        if (m_looperHeld[looper] && !m_looperClearTriggered[looper])
        {
            // Tap (no long-hold) — dispatch press-cycle command based on
            // current track state.
            publicTrack *pTrack = pTheLooper->getPublicTrack(looper);
            int ts = pTrack->getTrackState();
            int recorded = pTrack->getNumRecordedClips();

            if (recorded == 0 && !(ts & (TRACK_STATE_RECORDING | TRACK_STATE_PENDING_RECORD)))
            {
                // Empty looper → arm record (fallback; normally done on press).
                _queueCmd(ApcCmd::CLEAR_LAYER, looper);  // arg = track*NUM_LAYERS+0 = track
            }
            else if (ts & TRACK_STATE_RECORDING)
            {
                // Recording → finish + play.
                _queueCmd(ApcCmd::TRACK, looper);
            }
            else if (ts & TRACK_STATE_PLAYING)
            {
                // Playing → pause-at-cycle.
                _queueCmd(ApcCmd::STOP_TRACK, looper);
            }
            else if (ts & TRACK_STATE_STOPPED)
            {
                // Paused → play.
                _queueCmd(ApcCmd::TRACK, looper);
            }
            else
            {
                // Pending state: re-arm.
                _queueCmd(ApcCmd::TRACK, looper);
            }
        }
        m_looperHeld[looper] = false;
        return;
    }
    int preset = _presetFromPad(row, col);
    if (preset >= 0)
    {
        if (m_presetHeld[preset] && !m_presetCaptured[preset])
        {
            // Short-tap → restore if this preset has been captured.
            if (m_presetUsed[preset])
                _queueCmd(ApcCmd::PRESET_RESTORE, preset);
        }
        m_presetHeld[preset] = false;
        return;
    }
}

void apcKey25::_onButton(u8 note)
{
    if (note == APC_BTN_STOP_ALL)
        _queueCmd(ApcCmd::LOOPER, m_shift ? LOOP_COMMAND_STOP_IMMEDIATE : LOOP_COMMAND_STOP);
    else if (note == APC_BTN_RECORD)
        _queueCmd(ApcCmd::LOOPER, m_shift ? LOOP_COMMAND_ABORT_RECORDING : LOOP_COMMAND_DUB_MODE);
    else if (note == APC_BTN_PLAY)
        _queueCmd(ApcCmd::LOOPER, m_shift ? LOOP_COMMAND_LOOP_IMMEDIATE : LOOP_COMMAND_CLEAR_ALL);
    else if (note == APC_BTN_FORMAT)
    {
        m_liveEngaged = !m_liveEngaged;
        _applyLivePitch();
    }
}

void apcKey25::_capturePreset(int p)
{
    if (p < 0 || p >= LOOPER_NUM_PRESETS) return;
    u32 mask = 0;
    for (int n = 0; n < LOOPER_NUM_TRACKS && n < 32; n++)
    {
        publicTrack *pTrack = pTheLooper->getPublicTrack(n);
        if (!pTrack) continue;
        int ts = pTrack->getTrackState();
        // "Playing" includes RECORDING (which transitions to playing) and PLAYING.
        // Treat PENDING_PLAY as playing too; STOPPED / EMPTY count as not playing.
        if (ts & (TRACK_STATE_PLAYING | TRACK_STATE_PENDING_PLAY | TRACK_STATE_RECORDING | TRACK_STATE_PENDING_RECORD))
            mask |= (1u << n);
    }
    m_presetMask[p] = mask;
    m_presetUsed[p] = true;
}

void apcKey25::_applyPreset(int p)
{
    if (p < 0 || p >= LOOPER_NUM_PRESETS) return;
    if (!m_presetUsed[p]) return;
    u32 mask = m_presetMask[p];
    for (int n = 0; n < LOOPER_NUM_TRACKS && n < 32; n++)
    {
        publicTrack *pTrack = pTheLooper->getPublicTrack(n);
        if (!pTrack) continue;
        if (pTrack->getNumRecordedClips() == 0) continue;  // empty looper, skip
        bool shouldPlay = (mask & (1u << n)) != 0;
        int ts = pTrack->getTrackState();
        bool isPlaying = (ts & (TRACK_STATE_PLAYING | TRACK_STATE_PENDING_PLAY)) != 0;
        if (shouldPlay && !isPlaying)
            pTheLooper->command(LOOP_COMMAND_TRACK_BASE + n);
        else if (!shouldPlay && isPlaying)
            pTheLooper->command(LOOP_COMMAND_STOP_TRACK_BASE + n);
    }
}
