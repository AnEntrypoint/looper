#ifndef _gamepadState_h_
#define _gamepadState_h_

// Circle-free mirror of Circle's TGamePadState (circle/usb/usbgamepad.h) so the
// pure mapping helpers in gamepadInput.h and the host test compile without the
// Circle USB headers. On-device, gamepadInput.cpp includes the real Circle
// header; the field names/shape used by the pure helpers (axes[i].value /
// .minimum / .maximum, hats[], buttons, naxes/nhats/nbuttons) match exactly, so
// a real TGamePadState* is copied field-by-field into this mirror at the ISR
// snapshot boundary.
//
// When the real Circle header is present (compiling on-device) it defines
// TGamePadState already; this mirror is named GpState to avoid a clash and the
// snapshot copies into it.

#define GP_MAX_AXIS 16
#define GP_MAX_HATS 6

struct GpState {
    int naxes;
    struct { int value; int minimum; int maximum; } axes[GP_MAX_AXIS];
    int nhats;
    int hats[GP_MAX_HATS];
    int nbuttons;
    unsigned buttons;
};

#endif
