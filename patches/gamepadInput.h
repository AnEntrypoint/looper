#ifndef _gamepadInput_h_
#define _gamepadInput_h_

// USB gamepad -> looper control surface.
//
// A generic USB HID gamepad (Circle CUSBGamePadStandardDevice) drives the SAME
// logical controls the APC Key 25 drives, by synthesizing the MIDI events the
// APC would send and feeding them through pTheAPC->handleMidi / handleEffectsCC
// / handleFilterCC. No new DSP: every mapping reuses an existing, tested path.
//
// MAPPING (user spec):
//   Z-axis        -> transpose -12..+12 semitones   (CC52),   center deadzone
//   Z-rotation    -> formant shift                  (CC53),   center deadzone
//   X-axis vert.  -> push down = lowpass / up = highpass (CC55 / CC51), deadzone
//   X-axis horiz. -> resonance                      (CC54),   center deadzone
//   LT (trigger)  -> delay amount                   (CC49)
//   RT (trigger)  -> delay time                     (CC50)
//   R1 (bumper)   -> reverb MAX while held          (CC48 = 127 / 0)
//   L1 (bumper)   -> SHIFT (note APC_BTN_SHIFT)
//   remaining buttons -> loopers (pad notes, full record/finish/pause gesture)
//   HAT / dpad    -> glitch (microRepeat) repeat speeds (notes 82..86)
//
// CROSS-CORE: the device status handler fires in USB completion ISR context
// (Core 0). It MUST NOT touch apcKey25/effects (owned by Core 2). So the handler
// only SNAPSHOTS the raw TGamePadState into a coalescing latest-state buffer
// (ISR producer, lock-free publish-after-write); processTick() (Core 2, called
// from coreControlPlaneTick) reads the latest snapshot, diffs it against the
// last applied state, and emits the control changes. Gamepad state is LEVEL
// (absolute axis position), not edge, so coalescing-latest is correct for axes;
// for buttons/hat the diff against last-applied recovers every transition as
// long as both edges of a press are not coalesced inside one tick (Core 2 ticks
// ~1kHz; human presses are ms, so this is safe). A drop counter records
// snapshots overwritten before a tick consumed them (axes: harmless, latest
// wins; the rare button double-edge loss is bounded and counted).

#include "gamepadState.h"   // TGamePadState mirror (host-testable, Circle-free)
#include <string.h>

// ----------------------------------------------------------------------------
// Axis-index map. axes[] order is HID-report-descriptor declaration order
// (witness: usbgamepadstandard.cpp:259-291 -> axes[naxes++] in usage order).
// The COMMON layout is X=0, Y=1, Z=2, Rz=3 plus analog triggers; this is NOT
// guaranteed across devices, so these are NAMED TUNABLE CONSTANTS — a different
// pad is a one-line change here, never a redesign.
// ----------------------------------------------------------------------------
#ifndef GP_AXIS_X            // left stick horizontal  -> resonance
#define GP_AXIS_X            0
#endif
#ifndef GP_AXIS_Y            // left stick vertical    -> HP/LP filter
#define GP_AXIS_Y            1
#endif
#ifndef GP_AXIS_Z            // right stick horizontal -> transpose
#define GP_AXIS_Z            2
#endif
#ifndef GP_AXIS_RZ           // right stick vertical   -> formant
#define GP_AXIS_RZ           3
#endif
#ifndef GP_AXIS_LT           // analog left trigger    -> delay amount (-1 = none)
#define GP_AXIS_LT           4
#endif
#ifndef GP_AXIS_RT           // analog right trigger   -> delay time   (-1 = none)
#define GP_AXIS_RT           5
#endif

// Digital button bit masks (mirror of TGamePadButton from usbgamepad.h). Used
// when the device exposes a control as a digital button instead of an analog
// axis (triggers, dpad on some pads).
enum {
    GP_BTN_GUIDE  = 1u << 0,
    GP_BTN_LT     = 1u << 3,
    GP_BTN_RT     = 1u << 4,
    GP_BTN_L1     = 1u << 5,
    GP_BTN_R1     = 1u << 6,
    GP_BTN_Y      = 1u << 7,
    GP_BTN_B      = 1u << 8,
    GP_BTN_A      = 1u << 9,
    GP_BTN_X      = 1u << 10,
    GP_BTN_SELECT = 1u << 11,
    GP_BTN_L3     = 1u << 12,
    GP_BTN_R3     = 1u << 13,
    GP_BTN_START  = 1u << 14,
    GP_BTN_UP     = 1u << 15,
    GP_BTN_RIGHT  = 1u << 16,
    GP_BTN_DOWN   = 1u << 17,
    GP_BTN_LEFT   = 1u << 18,
    GP_BTN_PLUS   = 1u << 19,
    GP_BTN_MINUS  = 1u << 20,
    GP_BTN_TOUCH  = 1u << 21,
};

// Buttons consumed by named controls (NOT loopers). Everything else -> loopers.
static const unsigned GP_RESERVED_BUTTONS =
    GP_BTN_L1 | GP_BTN_R1 |                                // shift / reverb
    GP_BTN_LT | GP_BTN_RT |                                // delay amt/time (digital fallback)
    GP_BTN_UP | GP_BTN_DOWN | GP_BTN_LEFT | GP_BTN_RIGHT;  // dpad-as-buttons = glitch

// Per-axis center deadzone as a fraction of half-travel. "some deadzone" so a
// resting stick reads neutral and does not drift the parameter. Tunable.
#ifndef GP_DEADZONE_FRAC
#define GP_DEADZONE_FRAC     0.12f
#endif

// MIDI control numbers (must match apcKey25 handleEffectsCC / handleFilterCC).
enum {
    GP_CC_REVERB   = 48,
    GP_CC_DELAY    = 49,
    GP_CC_TIME     = 50,
    GP_CC_HP       = 51,
    GP_CC_TRANSPOSE= 52,
    GP_CC_FORMANT  = 53,
    GP_CC_RES      = 54,
    GP_CC_LP       = 55,
};

// APC constants needed for synthesized events (mirror of apcKey25.h).
#ifndef GP_APC_NOTE_ON
#define GP_APC_NOTE_ON   0x90
#endif
#ifndef GP_APC_NOTE_OFF
#define GP_APC_NOTE_OFF  0x80
#endif
#ifndef GP_APC_CC
#define GP_APC_CC        0xB0
#endif
#ifndef GP_APC_SHIFT_NOTE
#define GP_APC_SHIFT_NOTE 0x62
#endif

// Microrepeat latch notes (apcKey25 reads note-on 82..86). Hat directions map
// to four of these; held = latched, released = off.
static const int GP_HAT_NOTE_UP    = 82;   // 1 beat
static const int GP_HAT_NOTE_RIGHT = 83;   // 1/2
static const int GP_HAT_NOTE_DOWN  = 84;   // 1/4
static const int GP_HAT_NOTE_LEFT  = 85;   // 1/8

// ----------------------------------------------------------------------------
// Pure mapping helpers (host-testable, no Circle / no apcKey25 dependency).
// ----------------------------------------------------------------------------

// Normalize an axis value in [minimum,maximum] to 0.0..1.0. Robust to a
// degenerate range (min==max -> 0.5 neutral) and clamps out-of-range.
static inline float gpAxisNorm01(int value, int minimum, int maximum)
{
    if (maximum <= minimum) return 0.5f;               // degenerate -> center
    float t = (float)(value - minimum) / (float)(maximum - minimum);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

// Centered axis -> bipolar -1..+1 about center, with a center deadzone. Inside
// the deadzone returns exactly 0; outside, the value is rescaled so leaving the
// deadzone is monotonic with no jump (the deadzone edge maps to 0).
static inline float gpAxisBipolarDeadzoned(int value, int minimum, int maximum)
{
    float t = gpAxisNorm01(value, minimum, maximum);   // 0..1
    float c = (t - 0.5f) * 2.0f;                        // -1..+1
    float dz = GP_DEADZONE_FRAC;
    if (c >  dz) return (c - dz) / (1.0f - dz);
    if (c < -dz) return (c + dz) / (1.0f - dz);
    return 0.0f;
}

// Float 0..1 -> MIDI 0..127.
static inline int gpToMidi01(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    int m = (int)(v * 127.0f + 0.5f);
    if (m > 127) m = 127;
    if (m < 0) m = 0;
    return m;
}

// Bipolar -1..+1 -> MIDI 0..127 centered at 64 (for CC53 formant which centers
// on data2~64 with its own deadzone).
static inline int gpBipolarToMidi(float c)
{
    if (c < -1.0f) c = -1.0f;
    if (c >  1.0f) c =  1.0f;
    float f = 64.0f + c * 63.0f;
    int m = (int)(f + (f >= 0 ? 0.5f : -0.5f));   // round-half-away-from-zero
    if (m > 127) m = 127;
    if (m < 0) m = 0;
    return m;
}

// HID hat value (0..7 = N,NE,E,SE,S,SW,W,E... clockwise from up; >=8 or <0 =
// centered) -> a microrepeat note, or 0 for centered/none. We collapse the 8
// directions to the 4 cardinals (diagonals pick the dominant axis: up/down
// win on the vertical, then left/right).
static inline int gpHatToMicroNote(int hat)
{
    switch (hat) {
        case 0: return GP_HAT_NOTE_UP;     // N
        case 1: return GP_HAT_NOTE_UP;     // NE -> up
        case 2: return GP_HAT_NOTE_RIGHT;  // E
        case 3: return GP_HAT_NOTE_DOWN;   // SE -> down
        case 4: return GP_HAT_NOTE_DOWN;   // S
        case 5: return GP_HAT_NOTE_DOWN;   // SW -> down
        case 6: return GP_HAT_NOTE_LEFT;   // W
        case 7: return GP_HAT_NOTE_UP;     // NW -> up
        default: return 0;                 // centered / null
    }
}

// Dpad-as-buttons (no hat axis) -> microrepeat note. Vertical wins over
// horizontal so a diagonal still picks one division. 0 = none held.
static inline int gpDpadButtonsToMicroNote(unsigned buttons)
{
    if (buttons & GP_BTN_UP)    return GP_HAT_NOTE_UP;
    if (buttons & GP_BTN_DOWN)  return GP_HAT_NOTE_DOWN;
    if (buttons & GP_BTN_RIGHT) return GP_HAT_NOTE_RIGHT;
    if (buttons & GP_BTN_LEFT)  return GP_HAT_NOTE_LEFT;
    return 0;
}

// Map a "remaining" button bit position (0..31) to a looper track number, or
// -1 if that button is reserved/unused. Loopers are assigned in a stable order
// over the non-reserved buttons that exist on a typical pad.
static inline int gpButtonBitToLooper(int bit, int numTracks)
{
    unsigned mask = (1u << bit);
    if (mask & GP_RESERVED_BUTTONS) return -1;
    // Stable ordering: face + thumb + start/select + guide + +/-. Each gets the
    // next looper slot. Index into this table = looper track.
    static const unsigned order[] = {
        GP_BTN_A, GP_BTN_B, GP_BTN_X, GP_BTN_Y,
        GP_BTN_L3, GP_BTN_R3,
        GP_BTN_START, GP_BTN_SELECT, GP_BTN_GUIDE,
        GP_BTN_PLUS, GP_BTN_MINUS, GP_BTN_TOUCH,
    };
    for (int i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++)
        if (order[i] == mask) return (i < numTracks) ? i : -1;
    return -1;
}

// Map a looper track number to its APC pad note. The looper pads are cols 2-5,
// rows 0-4 (note = row*8 + col), index = row*4 + (col-2) in [0,20).
// (Mirror of apcKey25 _looperFromPad inverse.) Returns -1 if out of range.
static inline int gpLooperToPadNote(int looper, int numTracks)
{
    if (looper < 0 || looper >= numTracks) return -1;
    int row = looper / 4;
    int col = (looper % 4) + 2;
    return row * 8 + col;
}

// ----------------------------------------------------------------------------
// gamepadInput: ISR snapshot + Core-2 diff/emit. The emit step calls back
// through three function pointers so the header carries NO Circle/apcKey25
// dependency (the .cpp binds them to pTheAPC->handleMidi etc.). Host test binds
// them to recorders.
// ----------------------------------------------------------------------------
typedef void (*GpEmitMidiFn)(unsigned char status, unsigned char d1, unsigned char d2);

class gamepadInput {
public:
    gamepadInput()
    : m_emit(0), m_numTracks(20), m_connected(false), m_haveLast(false),
      m_dropped(0), m_lastButtons(0), m_lastHatNote(0), m_lastShift(false),
      m_lastReverb(false), m_pendSeq(0), m_applySeq(0)
    {
        memset((void *)&m_pending, 0, sizeof(m_pending));
        memset(&m_last,    0, sizeof(m_last));
        for (int i = 0; i < GP_MAX_AXIS; i++) m_lastAxisMidi[i] = -1;
    }

    void setEmit(GpEmitMidiFn fn)        { m_emit = fn; }
    void setNumTracks(int n)             { m_numTracks = n; }
    void setConnected(bool c)            { m_connected = c; if (!c) _resetMomentary(); }
    bool connected() const               { return m_connected; }
    unsigned dropped() const             { return m_dropped; }

    // ISR producer (Core 0). Coalescing latest-state snapshot: overwrite the
    // pending slot with the newest state. publish-after-write via a sequence
    // counter the consumer compares. If a prior pending was never consumed, the
    // overwrite is a coalesce (latest wins for axes); count it.
    void pushState(const GpState *s)
    {
        if (!s) return;
        if (m_pendSeq != m_applySeq) m_dropped++;   // un-consumed prior snapshot coalesced
        memcpy((void *)&m_pending, s, sizeof(GpState));   // copy (cast away volatile)
        // publish: bump seq last so the consumer sees a complete struct.
        m_pendSeq++;
    }

    // Core-2 consumer. Returns true if it applied a new snapshot this tick.
    bool processTick()
    {
        if (!m_connected || !m_emit) return false;
        if (m_pendSeq == m_applySeq) return false;   // nothing new
        GpState s;
        memcpy(&s, (const void *)&m_pending, sizeof(GpState));   // copy out (cast away volatile)
        m_applySeq = m_pendSeq;                       // mark consumed
        _apply(s);
        return true;
    }

    // Telemetry snapshot (Core 2 read).
    void telemetry(int *axes, unsigned *buttons, int *hatNote, unsigned *drops,
                   bool *shift, bool *reverb) const
    {
        if (axes)    *axes    = m_last.naxes;
        if (buttons) *buttons = m_last.buttons;
        if (hatNote) *hatNote = m_lastHatNote;
        if (drops)   *drops   = m_dropped;
        if (shift)   *shift   = m_lastShift;
        if (reverb)  *reverb  = m_lastReverb;
    }

private:
    void _emit(unsigned char st, unsigned char d1, unsigned char d2)
    { if (m_emit) m_emit(st, d1, d2); }

    // Emit a CC only when its MIDI bucket changed (edge-detect on axes) to avoid
    // flooding the handler every tick.
    void _ccIfChanged(int axisIdx, int cc, int midi)
    {
        if (axisIdx < 0 || axisIdx >= GP_MAX_AXIS) return;
        if (m_lastAxisMidi[axisIdx] == midi) return;
        m_lastAxisMidi[axisIdx] = midi;
        _emit(GP_APC_CC, (unsigned char)cc, (unsigned char)midi);
    }

    // Clear held/momentary controls so a yanked or idle pad never leaves SHIFT
    // stuck, reverb maxed, or a glitch latched.
    void _resetMomentary()
    {
        if (m_lastShift)  { _emit(GP_APC_NOTE_OFF, GP_APC_SHIFT_NOTE, 0); m_lastShift = false; }
        if (m_lastReverb) { _emit(GP_APC_CC, GP_CC_REVERB, 0);            m_lastReverb = false; }
        if (m_lastHatNote){ _emit(GP_APC_NOTE_OFF, (unsigned char)m_lastHatNote, 0); m_lastHatNote = 0; }
    }

    void _apply(const GpState &s)
    {
        // ---- Axes (level controls, bucket-edge-detected) -------------------
        // Z-axis -> transpose -12..+12 (CC52). Center deadzone -> 64 = 0 semis.
        if (GP_AXIS_Z < s.naxes) {
            float c = gpAxisBipolarDeadzoned(s.axes[GP_AXIS_Z].value,
                                             s.axes[GP_AXIS_Z].minimum,
                                             s.axes[GP_AXIS_Z].maximum);
            _ccIfChanged(GP_AXIS_Z, GP_CC_TRANSPOSE, gpBipolarToMidi(c));
        }
        // Z-rotation (Rz) -> formant (CC53). Center deadzone -> 64 neutral.
        if (GP_AXIS_RZ < s.naxes) {
            float c = gpAxisBipolarDeadzoned(s.axes[GP_AXIS_RZ].value,
                                             s.axes[GP_AXIS_RZ].minimum,
                                             s.axes[GP_AXIS_RZ].maximum);
            _ccIfChanged(GP_AXIS_RZ, GP_CC_FORMANT, gpBipolarToMidi(c));
        }
        // X-axis VERTICAL (Y) -> down=lowpass / up=highpass. We treat "down" as
        // the positive end of the raw axis (most pads report stick-down as max).
        // Down half: LP closes (CC55 127->0). Up half: HP opens (CC51 0->127).
        // In deadzone: LP open (127), HP off (0).
        if (GP_AXIS_Y < s.naxes) {
            float c = gpAxisBipolarDeadzoned(s.axes[GP_AXIS_Y].value,
                                             s.axes[GP_AXIS_Y].minimum,
                                             s.axes[GP_AXIS_Y].maximum);
            int lp = 127, hp = 0;
            if (c > 0.0f)      lp = gpToMidi01(1.0f - c);   // pushed DOWN -> close LP
            else if (c < 0.0f) hp = gpToMidi01(-c);          // pushed UP   -> open HP
            // two distinct CCs, distinct edge-state slots (Y for LP, reuse RT-1
            // slot index space is messy) -> dedicated last-midi members.
            if (m_lastLp != lp) { m_lastLp = lp; _emit(GP_APC_CC, GP_CC_LP, (unsigned char)lp); }
            if (m_lastHp != hp) { m_lastHp = hp; _emit(GP_APC_CC, GP_CC_HP, (unsigned char)hp); }
        }
        // X-axis HORIZONTAL (X) -> resonance (CC54). Bipolar magnitude: either
        // direction adds resonance (center deadzone = 0 res).
        if (GP_AXIS_X < s.naxes) {
            float c = gpAxisBipolarDeadzoned(s.axes[GP_AXIS_X].value,
                                             s.axes[GP_AXIS_X].minimum,
                                             s.axes[GP_AXIS_X].maximum);
            float mag = c < 0 ? -c : c;
            _ccIfChanged(GP_AXIS_X, GP_CC_RES, gpToMidi01(mag));
        }
        // LT/RT triggers -> delay amount / time. Analog axis if present, else
        // digital button fallback.
        {
            int ltMidi;
            if (GP_AXIS_LT < s.naxes)
                ltMidi = gpToMidi01(gpAxisNorm01(s.axes[GP_AXIS_LT].value,
                                                 s.axes[GP_AXIS_LT].minimum,
                                                 s.axes[GP_AXIS_LT].maximum));
            else
                ltMidi = (s.buttons & GP_BTN_LT) ? 127 : 0;
            _ccIfChanged(GP_AXIS_LT, GP_CC_DELAY, ltMidi);

            int rtMidi;
            if (GP_AXIS_RT < s.naxes)
                rtMidi = gpToMidi01(gpAxisNorm01(s.axes[GP_AXIS_RT].value,
                                                 s.axes[GP_AXIS_RT].minimum,
                                                 s.axes[GP_AXIS_RT].maximum));
            else
                rtMidi = (s.buttons & GP_BTN_RT) ? 127 : 0;
            _ccIfChanged(GP_AXIS_RT, GP_CC_TIME, rtMidi);
        }

        // ---- Buttons (edge-detected via diff vs m_lastButtons) -------------
        unsigned now = s.buttons;
        unsigned changed = now ^ m_lastButtons;

        // L1 -> SHIFT (momentary).
        if (changed & GP_BTN_L1) {
            bool down = (now & GP_BTN_L1) != 0;
            _emit(down ? GP_APC_NOTE_ON : GP_APC_NOTE_OFF, GP_APC_SHIFT_NOTE, down ? 127 : 0);
            m_lastShift = down;
        }
        // R1 -> reverb MAX (momentary).
        if (changed & GP_BTN_R1) {
            bool down = (now & GP_BTN_R1) != 0;
            _emit(GP_APC_CC, GP_CC_REVERB, down ? 127 : 0);
            m_lastReverb = down;
        }
        // Remaining buttons -> loopers. A press synthesizes a pad note-on (the
        // APC's _onPadPress drives the full record/finish/pause gesture incl.
        // backdate + grid-snap); release synthesizes a pad note-off (drives
        // _onPadRelease for the play/pause/resume tap path).
        for (int bit = 0; bit < 22; bit++) {
            unsigned m = (1u << bit);
            if (!(changed & m)) continue;
            int looper = gpButtonBitToLooper(bit, m_numTracks);
            if (looper < 0) continue;
            int note = gpLooperToPadNote(looper, m_numTracks);
            if (note < 0) continue;
            bool down = (now & m) != 0;
            _emit(down ? GP_APC_NOTE_ON : GP_APC_NOTE_OFF, (unsigned char)note, down ? 127 : 0);
        }
        m_lastButtons = now;

        // ---- HAT / dpad -> glitch (microRepeat) ----------------------------
        int hatNote = 0;
        if (s.nhats > 0)         hatNote = gpHatToMicroNote(s.hats[0]);
        if (hatNote == 0)        hatNote = gpDpadButtonsToMicroNote(now);   // dpad-as-buttons
        if (hatNote != m_lastHatNote) {
            if (m_lastHatNote)   _emit(GP_APC_NOTE_OFF, (unsigned char)m_lastHatNote, 0);
            if (hatNote)         _emit(GP_APC_NOTE_ON,  (unsigned char)hatNote, 127);
            m_lastHatNote = hatNote;
        }

        m_last = s;
        m_haveLast = true;
    }

    GpEmitMidiFn m_emit;
    int          m_numTracks;
    bool         m_connected;
    bool         m_haveLast;
    unsigned     m_dropped;

    // ISR<->Core2 coalescing snapshot.
    volatile GpState m_pending;
    volatile unsigned m_pendSeq;
    unsigned          m_applySeq;

    // Last-applied state for edge detection.
    GpState  m_last;
    int      m_lastAxisMidi[GP_MAX_AXIS];
    int      m_lastLp = 127;
    int      m_lastHp = 0;
    unsigned m_lastButtons;
    int      m_lastHatNote;
    bool     m_lastShift;
    bool     m_lastReverb;
};

// Global instance (defined in gamepadInput.cpp, allocated in audio.cpp setup()).
extern gamepadInput *pTheGamepad;

#endif
