#ifndef _apcKey25_h_
#define _apcKey25_h_

#include "commonDefines.h"
#include "Looper.h"

#define APC_ROWS            5
#define APC_COLS            8
#define APC_NOTE_ROW0       0x00
#define APC_NOTE_SHIFT      (APC_COLS)
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
#define APC_VEL_LED_ON      1

class apcKey25
{
public:
    apcKey25();

    void handleMidi(u8 status, u8 data1, u8 data2);
    void update();

private:
    bool m_shift;

    void _onPadPress(int row, int col);
    void _onButton(u8 note);
    void _sendLed(u8 note, u8 velocity);
    void _updateGridLeds();
    u8   _trackLedColor(int track);
    u8   _clipLedColor(int track, int clip);
    u8   _padNote(int row, int col);
};

extern apcKey25 *pTheAPC;

#endif
