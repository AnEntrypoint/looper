#define log_name "apc25"

#include "apcKey25.h"
#include "usbMidi.h"
#include <circle/logger.h>

apcKey25 *pTheAPC = 0;

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

    if (ts & TRACK_STATE_RECORDING)       return APC_VEL_LED_RED;
    if (ts & TRACK_STATE_PENDING_RECORD)  return APC_VEL_LED_YELLOW;
    if (ts & TRACK_STATE_PENDING_STOP)    return APC_VEL_LED_YELLOW;
    if (ts & TRACK_STATE_PENDING_PLAY)    return APC_VEL_LED_YELLOW;
    if (ts & TRACK_STATE_PLAYING)         return APC_VEL_LED_GREEN;
    if (ts & TRACK_STATE_STOPPED)         return APC_VEL_LED_OFF;
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

        bool anyMuted = false;
        for (int c = 0; c < LOOPER_NUM_LAYERS; c++)
            if (pTrack->getPublicClip(c)->isMuted()) { anyMuted = true; break; }

        _sendLed(_padNote(row, 4), anyMuted ? APC_VEL_LED_RED : _trackLedColor(row));

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

void apcKey25::_onPadPress(int row, int col)
{
    if (row >= LOOPER_NUM_TRACKS) return;

    if (col < LOOPER_NUM_LAYERS)
    {
        pTheLooper->command(LOOP_COMMAND_TRACK_BASE + row);
        return;
    }

    if (col == 4)
    {
        publicTrack *pTrack = pTheLooper->getPublicTrack(row);
        bool anyMuted = false;
        for (int c = 0; c < LOOPER_NUM_LAYERS; c++)
            if (pTrack->getPublicClip(c)->isMuted()) { anyMuted = true; break; }
        for (int c = 0; c < LOOPER_NUM_LAYERS; c++)
        {
            pTrack->getPublicClip(c)->setMute(!anyMuted);
            usbMidiSendCC(CLIP_MUTE_BASE_CC + row * LOOPER_NUM_LAYERS + c, !anyMuted ? 1 : 0);
        }
        return;
    }

    if (col == 5)
    {
        pTheLooper->command(LOOP_COMMAND_ERASE_TRACK_BASE + row);
        return;
    }

    if (col == 6)
    {
        pTheLooper->command(LOOP_COMMAND_SET_LOOP_START);
        return;
    }

    if (col == 7)
    {
        pTheLooper->command(LOOP_COMMAND_CLEAR_LOOP_START);
        return;
    }
}

void apcKey25::_onButton(u8 note)
{
    if (note == APC_BTN_STOP_ALL)
    {
        pTheLooper->command(m_shift ? LOOP_COMMAND_STOP_IMMEDIATE : LOOP_COMMAND_STOP);
        return;
    }
    if (note == APC_BTN_RECORD)
    {
        pTheLooper->command(m_shift ? LOOP_COMMAND_ABORT_RECORDING : LOOP_COMMAND_DUB_MODE);
        return;
    }
    if (note == APC_BTN_PLAY)
    {
        pTheLooper->command(m_shift ? LOOP_COMMAND_LOOP_IMMEDIATE : LOOP_COMMAND_CLEAR_ALL);
        return;
    }
}

void apcKey25::handleMidi(u8 status, u8 data1, u8 data2)
{
    CLogger::Get()->Write(log_name, LogNotice, "MIDI rx: %02X %02X %02X", status, data1, data2);
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
    _updateGridLeds();
}
