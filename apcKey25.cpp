#define log_name "apc25"

#include "apcKey25.h"
#include "usbMidi.h"
#include <circle/logger.h>

apcKey25 *pTheAPC = 0;

// Command queue for safe dispatch from interrupt to main loop
static volatile u16 s_pendingCmd = LOOP_COMMAND_NONE;
static volatile u8  s_pendingMuteTrack = 0xFF;

apcKey25::apcKey25() : m_shift(false)
{
    pTheAPC = this;
}

u8 apcKey25::_padNote(int row, int col)
{
    return (u8)(row * APC_COLS + col);
}

void apcKey25::_sendLed(u8 note, u8 velocity)
{
    usbMidiSendNoteOn(note, velocity);
}

u8 apcKey25::_trackLedColor(int track)
{
    if (track >= LOOPER_NUM_TRACKS) return APC_VEL_LED_OFF;

    publicTrack *pTrack = pTheLooper->getPublicTrack(track);
    u16 ts = pTrack->getTrackState();

    if (ts & TRACK_STATE_RECORDING)      return APC_VEL_LED_RED;
    if (ts & TRACK_STATE_PENDING_RECORD) return APC_VEL_LED_YELLOW;
    if (ts & TRACK_STATE_PENDING_STOP)   return APC_VEL_LED_YELLOW;
    if (ts & TRACK_STATE_PENDING_PLAY)   return APC_VEL_LED_YELLOW;
    if (ts & TRACK_STATE_PLAYING)        return APC_VEL_LED_GREEN;
    if (ts & TRACK_STATE_STOPPED)        return APC_VEL_LED_OFF;
    return APC_VEL_LED_OFF;
}

u8 apcKey25::_clipLedColor(int track, int clip)
{
    if (track >= LOOPER_NUM_TRACKS || clip >= LOOPER_NUM_LAYERS)
        return APC_VEL_LED_OFF;

    publicTrack *pTrack = pTheLooper->getPublicTrack(track);
    publicClip  *pClip  = pTrack->getPublicClip(clip);
    u16 state = pClip->getClipState();
    u16 ts    = pTrack->getTrackState();

    if (pClip->isMuted()) return APC_VEL_LED_RED;

    if (state & (CLIP_STATE_RECORD_IN | CLIP_STATE_RECORD_MAIN | CLIP_STATE_RECORD_END))
        return (ts & TRACK_STATE_PENDING_STOP) ? APC_VEL_LED_YELLOW : APC_VEL_LED_RED;

    if (state & (CLIP_STATE_PLAY_MAIN | CLIP_STATE_PLAY_END))
        return (ts & TRACK_STATE_PENDING_STOP) ? APC_VEL_LED_YELLOW : APC_VEL_LED_GREEN;

    if (state & CLIP_STATE_RECORDED)
        return (ts & TRACK_STATE_PENDING_PLAY) ? APC_VEL_LED_YELLOW : APC_VEL_LED_OFF;

    return (ts & TRACK_STATE_PENDING_RECORD) ? APC_VEL_LED_YELLOW : APC_VEL_LED_OFF;
}

void apcKey25::_updateGridLeds()
{
    for (int row = 0; row < LOOPER_NUM_TRACKS; row++)
    {
        publicTrack *pTrack = pTheLooper->getPublicTrack(row);

        for (int col = 0; col < LOOPER_NUM_LAYERS; col++)
            _sendLed(_padNote(row, col), _clipLedColor(row, col));

        // Col 4: track button — shows track record/play/stop state
        _sendLed(_padNote(row, 4), _trackLedColor(row));

        // Col 5: erase — yellow if track has recorded content
        u8 eraseColor = (pTrack->getNumRecordedClips() > 0) ? APC_VEL_LED_YELLOW : APC_VEL_LED_OFF;
        _sendLed(_padNote(row, 5), eraseColor);

        _sendLed(_padNote(row, 6), APC_VEL_LED_OFF);
        _sendLed(_padNote(row, 7), APC_VEL_LED_OFF);
    }

    bool running = pTheLooper->getRunning();
    u16  pending = pTheLooper->getPendingCommand();
    _sendLed(APC_BTN_STOP_ALL, running ? (pending == LOOP_COMMAND_STOP ? APC_VEL_LED_YELLOW : APC_VEL_LED_GREEN) : APC_VEL_LED_OFF);
    _sendLed(APC_BTN_RECORD,   pTheLooper->getDubMode() ? APC_VEL_LED_RED : APC_VEL_LED_OFF);
    _sendLed(APC_BTN_PLAY,     m_shift ? APC_VEL_LED_YELLOW : APC_VEL_LED_OFF);
}

// Called from interrupt — only queue, never call looper or SendPlainMIDI
void apcKey25::_onPadPress(int row, int col)
{
    if (row >= LOOPER_NUM_TRACKS) return;

    if (col < LOOPER_NUM_LAYERS)
    {
        s_pendingCmd = LOOP_COMMAND_TRACK_BASE + row;
        return;
    }

    if (col == 4)
    {
        // RC-505 style track button: command looper for this track
        s_pendingCmd = LOOP_COMMAND_TRACK_BASE + row;
        return;
    }

    if (col == 5)
    {
        s_pendingCmd = LOOP_COMMAND_ERASE_TRACK_BASE + row;
        return;
    }

    if (col == 6)
    {
        s_pendingCmd = LOOP_COMMAND_SET_LOOP_START;
        return;
    }

    if (col == 7)
    {
        s_pendingCmd = LOOP_COMMAND_CLEAR_LOOP_START;
        return;
    }
}

// Called from interrupt — only queue
void apcKey25::_onButton(u8 note)
{
    if (note == APC_BTN_STOP_ALL)
    {
        s_pendingCmd = m_shift ? LOOP_COMMAND_STOP_IMMEDIATE : LOOP_COMMAND_STOP;
        return;
    }
    if (note == APC_BTN_RECORD)
    {
        s_pendingCmd = m_shift ? LOOP_COMMAND_ABORT_RECORDING : LOOP_COMMAND_DUB_MODE;
        return;
    }
    if (note == APC_BTN_PLAY)
    {
        s_pendingCmd = m_shift ? LOOP_COMMAND_LOOP_IMMEDIATE : LOOP_COMMAND_CLEAR_ALL;
        return;
    }
}

// Called from interrupt context — only set flags, never call SendPlainMIDI or looper
void apcKey25::handleMidi(u8 status, u8 data1, u8 data2)
{
    u8 msgType = status & 0xF0;

    if (msgType == APC_CH_NOTE_ON && data2 > 0)
    {
        if (data1 == APC_BTN_SHIFT)
        {
            m_shift = true;
            return;
        }
        if (data1 < APC_ROWS * APC_COLS)
        {
            int row = data1 / APC_COLS;
            int col = data1 % APC_COLS;
            _onPadPress(row, col);
            return;
        }
        _onButton(data1);
        return;
    }

    if (msgType == APC_CH_NOTE_OFF || (msgType == APC_CH_NOTE_ON && data2 == 0))
    {
        if (data1 == APC_BTN_SHIFT)
            m_shift = false;
        return;
    }
}

void apcKey25::update()
{
    if (!pTheLooper) return;

    // Dispatch any queued command from interrupt
    u16 cmd = s_pendingCmd;
    if (cmd != LOOP_COMMAND_NONE)
    {
        s_pendingCmd = LOOP_COMMAND_NONE;
        pTheLooper->command(cmd);
    }

    _updateGridLeds();
}
