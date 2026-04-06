#ifndef _apcKey25_h_
#define _apcKey25_h_

#include "commonDefines.h"
#include "Looper.h"

#define APC_ROWS            5
#define APC_COLS            8

// APC Key 25 pad notes: row*8 + col
#define APC_BTN_STOP_ALL    0x51
#define APC_BTN_PLAY        0x5B
#define APC_BTN_RECORD      0x5D
#define APC_BTN_SHIFT       0x62

#define APC_CH_NOTE_ON      0x90
#define APC_CH_NOTE_OFF     0x80
#define APC_CH_CC           0xB0

#define APC_VEL_LED_OFF     0
#define APC_VEL_LED_GREEN   1
#define APC_VEL_LED_RED     3
#define APC_VEL_LED_YELLOW  5

// Triple-tap / hold erase: 3 presses within this many update() calls (~300ms at 100Hz)
#define APC_ERASE_TAP_WINDOW_MS   500
#define APC_HOLD_ERASE_MS         1000

class apcKey25
{
public:
    apcKey25();

    void handleMidi(u8 status, u8 data1, u8 data2);
    void update();

private:
    bool m_shift;

    // Queued command from interrupt
    volatile u16 m_pendingCmd;

    // Triple-tap erase tracking per track (col0 presses)
    unsigned long m_tapTime[LOOPER_NUM_TRACKS];
    int           m_tapCount[LOOPER_NUM_TRACKS];

    // Hold-to-erase on col1 (mute button)
    unsigned long m_col1HoldStart[LOOPER_NUM_TRACKS];
    bool          m_col1Held[LOOPER_NUM_TRACKS];
    bool          m_col1EraseTriggered[LOOPER_NUM_TRACKS];

    unsigned long m_nowMs;  // updated each update() call

    void _onPadPress(int row, int col);
    void _onPadRelease(int row, int col);
    void _onButton(u8 note);
    void _sendLed(u8 note, u8 velocity);
    void _updateGridLeds();
    u8   _trackLedColor(int track);   // col0 color
    u8   _muteLedColor(int track);    // col1 color
    u8   _padNote(int row, int col);
};

extern apcKey25 *pTheAPC;

#endif
