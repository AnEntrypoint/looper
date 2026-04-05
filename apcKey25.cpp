#define log_name "apc25"

#include "apcKey25.h"
#include <circle/logger.h>

extern void sendSerialMidiNoteOn(u8 note, u8 velocity);

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
    sendSerialMidiNoteOn(note, velocity);
}

u8 apcKey25::_clipLedColor(int track, int clip)
{
    if (track >= LOOPER_NUM_TRACKS || clip >= LOOPER_NUM_LAYERS)
        return APC_VEL_LED_OFF;

    publicTrack *pTrack = pTheLooper->getPublicTrack(track);
    publicClip  *pClip  = pTrack->getPublicClip(clip);
    u16 state = pClip->getClipState();
    u16 trackState = pTrack->getTrackState();

    if (pClip->isMuted())
        return APC_VEL_LED_RED;

    if (state & (CLIP_STATE_RECORD_IN | CLIP_STATE_RECORD_MAIN | CLIP_STATE_RECORD_END))
    {
        u16 pending = pTheLooper->getPendingCommand();
        if (pending == LOOP_COMMAND_STOP || pending == LOOP_COMMAND_STOP_IMMEDIATE)
            return APC_VEL_LED_YELLOW;
        return APC_VEL_LED_RED;
    }

    if (state & (CLIP_STATE_PLAY_MAIN | CLIP_STATE_PLAY_END))
    {
        if (trackState & TRACK_STATE_PENDING_STOP)
            return APC_VEL_LED_YELLOW;
        return APC_VEL_LED_GREEN;
    }

    if (state & CLIP_STATE_RECORDED)
    {
        if (trackState & TRACK_STATE_PENDING_PLAY)
            return APC_VEL_LED_YELLOW;
        return APC_VEL_LED_OFF;
    }

    if (trackState & TRACK_STATE_PENDING_RECORD)
        return APC_VEL_LED_YELLOW;

    return APC_VEL_LED_OFF;
}

void apcKey25::_updateGridLeds()
{
    for (int row = 0; row < LOOPER_NUM_TRACKS; row++)
    {
        publicTrack *pTrack = pTheLooper->getPublicTrack(row);

        for (int col = 0; col < LOOPER_NUM_LAYERS; col++)
        {
            u8 color = _clipLedColor(row, col);
            _sendLed(_padNote(row, col), color);
        }

        bool selected = pTrack->isSelected();
        u8 muteNote = _padNote(row, 4);
        bool anyMuted = false;
        for (int c = 0; c < LOOPER_NUM_LAYERS; c++)
            if (pTrack->getPublicClip(c)->isMuted()) { anyMuted = true; break; }
        _sendLed(muteNote, anyMuted ? APC_VEL_LED_RED : (selected ? APC_VEL_LED_GREEN : APC_VEL_LED_OFF));
    }
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
            sendSerialMidiCC(CLIP_MUTE_BASE_CC + row * LOOPER_NUM_LAYERS + c, !anyMuted ? 1 : 0);
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
    if (note == APC_BTN_PLAY)
    {
        if (m_shift)
            pTheLooper->command(LOOP_COMMAND_CLEAR_ALL);
        else
        {
            int sel = pTheLooper->getSelectedTrackNum();
            if (sel >= 0)
                pTheLooper->command(LOOP_COMMAND_TRACK_BASE + sel);
        }
        return;
    }
    if (note == APC_BTN_RECORD)
    {
        if (m_shift)
            pTheLooper->command(LOOP_COMMAND_DUB_MODE);
        else
        {
            int sel = pTheLooper->getSelectedTrackNum();
            if (sel >= 0)
                pTheLooper->command(LOOP_COMMAND_TRACK_BASE + sel);
        }
        return;
    }
}

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
    _updateGridLeds();
}
