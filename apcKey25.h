#ifndef _apcKey25_h_
#define _apcKey25_h_

#include "commonDefines.h"
#include "Looper.h"

// APC Key 25 pad grid: 5 rows x 8 cols, note = row*8 + col, row 0 = bottom
#define APC_ROWS            5
#define APC_COLS            8

#define APC_BTN_STOP_ALL    0x51
#define APC_BTN_PLAY        0x5B
#define APC_BTN_RECORD      0x5D
#define APC_BTN_SHIFT       0x62

#define APC_CH_NOTE_ON      0x90
#define APC_CH_NOTE_OFF     0x80

#define APC_VEL_LED_OFF     0
#define APC_VEL_LED_GREEN   1
#define APC_VEL_LED_RED     3
#define APC_VEL_LED_YELLOW  5

#define APC_HOLD_ERASE_MS   1000
#define APC_LED_BOOT_DELAY_MS 2000

// We map looper tracks to APC rows from bottom: track 0 = row 0 (bottom row)
// Col 0 = rec/play/stop track button
// Col 1 = track presence (tap to erase, hold to erase)

struct ApcCmd {
    enum Type { NONE, TRACK, ERASE_TRACK, STOP_TRACK, LOOPER } type;
    int arg;
};

class apcKey25
{
public:
    apcKey25();

    void handleMidi(u8 status, u8 data1, u8 data2);
    void update();

private:
    bool          m_shift;
    volatile bool m_cmdReady;
    volatile ApcCmd::Type m_cmdType;
    volatile int  m_cmdArg;

    // Hold tracking for col1 (mute/erase)
    unsigned long m_col1HoldStart[LOOPER_NUM_TRACKS];
    bool          m_col1Held[LOOPER_NUM_TRACKS];
    bool          m_col1EraseTriggered[LOOPER_NUM_TRACKS];

    unsigned long m_nowMs;
    unsigned long m_bootMs;
    unsigned long m_lastLedMs;

    void _queueCmd(ApcCmd::Type type, int arg);
    void _onPadPress(int row, int col);
    void _onPadRelease(int row, int col);
    void _onButton(u8 note);
    void _sendLed(u8 note, u8 velocity);
    void _updateGridLeds();
    u8   _trackLedColor(int track);
    u8   _muteLedColor(int track);
    u8   _padNote(int row, int col);
};

extern apcKey25 *pTheAPC;

#endif
