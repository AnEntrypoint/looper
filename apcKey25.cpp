#define log_name "apc25"

#include "apcKey25.h"
#include "usbMidi.h"
#include <circle/logger.h>
#include <circle/timer.h>

apcKey25 *pTheAPC = 0;

apcKey25::apcKey25() : m_shift(false), m_pendingCmd(LOOP_COMMAND_NONE), m_nowMs(0)
{
    pTheAPC = this;
    for (int i = 0; i < LOOPER_NUM_TRACKS; i++)
    {
        m_tapTime[i]            = 0;
        m_tapCount[i]           = 0;
        m_col1HoldStart[i]      = 0;
        m_col1Held[i]           = false;
        m_col1EraseTriggered[i] = false;
    }
}

u8 apcKey25::_padNote(int row, int col)
{
    return (u8)(row * APC_COLS + col);
}

void apcKey25::_sendLed(u8 note, u8 velocity)
{
    usbMidiSendNoteOn(note, velocity);
}

// Col 0: rec/play/overdub state
u8 apcKey25::_trackLedColor(int track)
{
    if (track >= LOOPER_NUM_TRACKS) return APC_VEL_LED_OFF;

    publicTrack *pTrack = pTheLooper->getPublicTrack(track);
    u16 ts = pTrack->getTrackState();

    if (ts & TRACK_STATE_RECORDING)
    {
        // Red = recording base, yellow = overdubbing (dub mode)
        return pTheLooper->getDubMode() ? APC_VEL_LED_YELLOW : APC_VEL_LED_RED;
    }
    if (ts & (TRACK_STATE_PENDING_RECORD | TRACK_STATE_PENDING_PLAY | TRACK_STATE_PENDING_STOP))
        return APC_VEL_LED_YELLOW;
    if (ts & TRACK_STATE_PLAYING)
        return APC_VEL_LED_GREEN;
    return APC_VEL_LED_OFF;
}

// Col 1: has-clip indicator — green = has clip (unmuted), red = muted, off = empty
u8 apcKey25::_muteLedColor(int track)
{
    if (track >= LOOPER_NUM_TRACKS) return APC_VEL_LED_OFF;

    publicTrack *pTrack = pTheLooper->getPublicTrack(track);
    if (pTrack->getNumRecordedClips() == 0) return APC_VEL_LED_OFF;

    // Check if any clip is muted
    for (int c = 0; c < LOOPER_NUM_LAYERS; c++)
    {
        if (pTrack->getPublicClip(c)->isMuted())
            return APC_VEL_LED_RED;
    }
    return APC_VEL_LED_GREEN;
}

void apcKey25::_updateGridLeds()
{
    for (int row = 0; row < LOOPER_NUM_TRACKS; row++)
    {
        _sendLed(_padNote(row, 0), _trackLedColor(row));
        _sendLed(_padNote(row, 1), _muteLedColor(row));
        // cols 2-7 off for now
        for (int col = 2; col < APC_COLS; col++)
            _sendLed(_padNote(row, col), APC_VEL_LED_OFF);
    }

    bool running = pTheLooper->getRunning();
    u16  pending = pTheLooper->getPendingCommand();
    _sendLed(APC_BTN_STOP_ALL, running ? (pending == LOOP_COMMAND_STOP ? APC_VEL_LED_YELLOW : APC_VEL_LED_GREEN) : APC_VEL_LED_OFF);
    _sendLed(APC_BTN_RECORD,   pTheLooper->getDubMode() ? APC_VEL_LED_RED : APC_VEL_LED_OFF);
    _sendLed(APC_BTN_PLAY,     m_shift ? APC_VEL_LED_YELLOW : APC_VEL_LED_OFF);
}

// All _on* called from interrupt — only set flags
void apcKey25::_onPadPress(int row, int col)
{
    if (row >= LOOPER_NUM_TRACKS) return;

    if (col == 0)
    {
        // Triple-tap erase detection (tracked in update())
        m_pendingCmd = LOOP_COMMAND_TRACK_BASE + row;
        // Signal a tap for erase detection — use high bit of tapCount as dirty flag
        m_tapCount[row] |= 0x100;
        return;
    }

    if (col == 1)
    {
        // Hold-to-erase: mark hold start; mute toggle on release (if not erased)
        m_col1Held[row]           = true;
        m_col1EraseTriggered[row] = false;
        m_col1HoldStart[row]      = m_nowMs;
        return;
    }
}

void apcKey25::_onPadRelease(int row, int col)
{
    if (row >= LOOPER_NUM_TRACKS) return;

    if (col == 1)
    {
        if (m_col1Held[row] && !m_col1EraseTriggered[row])
        {
            // Short press = mute toggle
            m_pendingCmd = LOOP_COMMAND_TRACK_BASE + 0x80 + row; // encode as mute toggle
        }
        m_col1Held[row] = false;
    }
}

void apcKey25::_onButton(u8 note)
{
    if (note == APC_BTN_STOP_ALL)
    {
        m_pendingCmd = m_shift ? LOOP_COMMAND_STOP_IMMEDIATE : LOOP_COMMAND_STOP;
        return;
    }
    if (note == APC_BTN_RECORD)
    {
        m_pendingCmd = m_shift ? LOOP_COMMAND_ABORT_RECORDING : LOOP_COMMAND_DUB_MODE;
        return;
    }
    if (note == APC_BTN_PLAY)
    {
        m_pendingCmd = m_shift ? LOOP_COMMAND_LOOP_IMMEDIATE : LOOP_COMMAND_CLEAR_ALL;
        return;
    }
}

void apcKey25::handleMidi(u8 status, u8 data1, u8 data2)
{
    u8 msgType = status & 0xF0;

    if (msgType == APC_CH_NOTE_ON && data2 > 0)
    {
        if (data1 == APC_BTN_SHIFT) { m_shift = true; return; }
        if (data1 < APC_ROWS * APC_COLS)
        {
            _onPadPress(data1 / APC_COLS, data1 % APC_COLS);
            return;
        }
        _onButton(data1);
        return;
    }

    if (msgType == APC_CH_NOTE_OFF || (msgType == APC_CH_NOTE_ON && data2 == 0))
    {
        if (data1 == APC_BTN_SHIFT) { m_shift = false; return; }
        if (data1 < APC_ROWS * APC_COLS)
            _onPadRelease(data1 / APC_COLS, data1 % APC_COLS);
        return;
    }
}

void apcKey25::update()
{
    if (!pTheLooper) return;

    m_nowMs = CTimer::Get()->GetClockTicks() / 1000;

    // Dispatch queued command
    u16 cmd = m_pendingCmd;
    if (cmd != LOOP_COMMAND_NONE)
    {
        m_pendingCmd = LOOP_COMMAND_NONE;

        // Mute toggle encoded as TRACK_BASE + 0x80 + row
        if (cmd >= (LOOP_COMMAND_TRACK_BASE + 0x80) &&
            cmd <  (LOOP_COMMAND_TRACK_BASE + 0x80 + LOOPER_NUM_TRACKS))
        {
            int row = cmd - (LOOP_COMMAND_TRACK_BASE + 0x80);
            publicTrack *pTrack = pTheLooper->getPublicTrack(row);
            bool anyMuted = false;
            for (int c = 0; c < LOOPER_NUM_LAYERS; c++)
                if (pTrack->getPublicClip(c)->isMuted()) { anyMuted = true; break; }
            for (int c = 0; c < LOOPER_NUM_LAYERS; c++)
                pTrack->getPublicClip(c)->setMute(!anyMuted);
        }
        else
        {
            // Triple-tap erase: clear the dirty flag, count the tap
            if (cmd >= LOOP_COMMAND_TRACK_BASE && cmd < LOOP_COMMAND_TRACK_BASE + LOOPER_NUM_TRACKS)
            {
                int row = cmd - LOOP_COMMAND_TRACK_BASE;
                if (m_tapCount[row] & 0x100)
                {
                    m_tapCount[row] &= ~0x100;
                    unsigned long now = m_nowMs;
                    if (now - m_tapTime[row] > APC_ERASE_TAP_WINDOW_MS)
                        m_tapCount[row] = 1;
                    else
                        m_tapCount[row]++;
                    m_tapTime[row] = now;

                    if (m_tapCount[row] >= 3)
                    {
                        m_tapCount[row] = 0;
                        pTheLooper->command(LOOP_COMMAND_ERASE_TRACK_BASE + row);
                        _updateGridLeds();
                        return;
                    }
                }
            }
            pTheLooper->command(cmd);
        }
    }

    // Hold-to-erase on col1
    for (int row = 0; row < LOOPER_NUM_TRACKS; row++)
    {
        if (m_col1Held[row] && !m_col1EraseTriggered[row])
        {
            if (m_nowMs - m_col1HoldStart[row] >= APC_HOLD_ERASE_MS)
            {
                m_col1EraseTriggered[row] = true;
                pTheLooper->command(LOOP_COMMAND_ERASE_TRACK_BASE + row);
            }
        }
    }

    _updateGridLeds();
}
