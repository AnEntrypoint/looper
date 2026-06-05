// midiMap.h — Atomic, controller-agnostic MIDI mapping.
//
// PURPOSE
// -------
// Every MIDI control the looper understands — input (a knob/pad/button the
// musician moves) and output (an LED / feedback the firmware lights) — is
// described here as an INDEPENDENT, SELF-CONTAINED record. Nothing in this
// file assumes the APC Key 25's note numbers, CC numbers, channel scheme, or
// LED velocity colours: those are just the values of the DEFAULT profile
// (g_apc25Profile) shipped at the bottom. Swap the profile and a different
// controller drives the same looper with its own addresses, its own LED
// scheme, and its own knob nuance — no change to the looper's logic.
//
// This is the SINGLE FILE the request asks for: "take each and every midi
// control for in and out and map them as independent atomic configs in a file
// so midi can be remapped for other controllers."
//
// ATOMICITY
// ---------
// One MidiInputMap row = one control. Its key is the full (statusType,
// channel, data1-range) triple so a remap can move it anywhere, and so a
// controller that does NOT use the APC's multi-channel pitch scheme can put
// everything on one channel without ambiguity. Its value is a LOGICAL
// MidiAction (what the looper should DO), never a hard-coded APC dispatch.
// One MidiOutputMap row = one feedback state -> one MIDI emission.
//
// CONTROLLER NUANCE
// -----------------
// - valueMode lets a row interpret data2 as APC-style absolute 0..127, or as a
//   relative/endless encoder delta (two's-complement or sign-bit), so endless
//   encoders are supported without forcing the APC absolute style.
// - The LED scheme is data: a controller with no colour velocities, or with a
//   different colour table, supplies its own MidiOutputMap. A missing output
//   row degrades to silence (no LED), never a crash or a stuck pad.
// - data1Lo/data1Hi let one row cover a contiguous block (e.g. the 40 grid
//   pads) so a controller with a different grid size just changes the bounds.
//
// OPERATIONAL INVARIANT
// ---------------------
// The default APC25 profile here reproduces the existing hard-coded behavior
// byte-for-byte. midiMapResolveInput() returns the SAME logical action the
// current handleMidi/_onButton/handleFilterCC/handleEffectsCC code takes for
// every (status,data1,data2) the APC emits; midiMapResolveOutput() returns the
// SAME note+velocity _updateGridLeds sends for every looper/preset state.
// Witnessed by scripts/test-midi-config-parity.cpp (ALL PASS).
//
// STORAGE
// -------
// The profile is a compiled-in table (zero new deps — no JSON/parser bloat,
// per the maintenance-surface rule). FatFs is mounted on this firmware
// (patches/kernel.cpp f_mount), so a future SD override (SD:/firmware/
// midimap.txt) can replace g_activeProfile at init without touching callers —
// the indirection that makes that possible already lives here.

#ifndef MIDIMAP_H
#define MIDIMAP_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Logical actions — what a control DOES, independent of how it is addressed.
// ---------------------------------------------------------------------------
enum MidiAction {
    MA_NONE = 0,            // unmapped -> ignore cleanly (current fallthrough)

    // Grid / transport (param carries the looper index or a sub-selector)
    MA_PAD,                // a grid pad press/release at param=pad-number
    MA_BTN_STOP_ALL,       // global stop (shift variant chosen by shiftHeld)
    MA_BTN_RECORD,         // global record/dub  (shift -> abort)
    MA_BTN_PLAY,           // global play/clear   (shift -> loop-immediate / clear-all)
    MA_BTN_TOGGLE_LIVE,    // toggle live-pitch engage (FORMAT button / ch0 note64)
    MA_SHIFT,              // SHIFT modifier (held = monitor mode + chord modifier)

    // Live pitch (param unused; value drives the semitone target)
    MA_LIVE_PITCH_NOTE_CH1,// note number -> semitones, engages (channel 1)
    MA_LIVE_PITCH_NOTE_CH2,// note number -> semitones, always engages (channel 2)
    MA_LIVE_PITCH_MODWHEEL,// CC1: deadzone disengages, else +/-12 st
    MA_LIVE_PITCH_CC,      // CC52: linear 0..127 -> +/-12 st

    // Filters / effects / formant (param = which knob)
    MA_FILTER_CC,          // forwards to handleFilterCC(param,...)
    MA_EFFECT_CC,          // forwards to handleEffectsCC(param,...)

    // Engine tuning (UDP-inject only; param = the tuning knob id 100..107)
    MA_ENGINE_TUNE_CC,

    MA_ACTION_COUNT
};

// How a control's data2 byte is interpreted.
enum MidiValueMode {
    MV_ABSOLUTE = 0,       // 0..127 absolute (APC knobs, note velocity)
    MV_RELATIVE_TWOS,      // endless encoder: 64-relative two's-complement delta
    MV_RELATIVE_SIGNBIT,   // endless encoder: bit6 = sign, bits0..5 = magnitude
    MV_TRIGGER             // value ignored (a button: presence is the event)
};

// MIDI status nibble classes (channel-independent).
enum MidiStatusType {
    MS_NOTE_ON  = 0x90,
    MS_NOTE_OFF = 0x80,
    MS_CC       = 0xB0
};

// ---------------------------------------------------------------------------
// One atomic INPUT control.
// Matches an incoming (status&0xF0, status&0x0F, data1) where
//   statusType == ev.statusType  AND
//   (channel == MIDI_ANY_CHANNEL OR channel == ev.channel)  AND
//   data1Lo <= ev.data1 <= data1Hi.
// ---------------------------------------------------------------------------
static const int MIDI_ANY_CHANNEL = -1;

struct MidiInputMap {
    uint8_t  statusType;   // MS_NOTE_ON / MS_NOTE_OFF / MS_CC
    int8_t   channel;      // 0..15 or MIDI_ANY_CHANNEL
    uint8_t  data1Lo;      // inclusive low note/CC number
    uint8_t  data1Hi;      // inclusive high note/CC number (==Lo for a single)
    uint8_t  valueMode;    // MidiValueMode
    uint8_t  action;       // MidiAction
    int16_t  param;        // action selector (knob id, or -1 = "use data1")
};

// ---------------------------------------------------------------------------
// One atomic OUTPUT control: a logical feedback state -> a MIDI emission.
// Logical states are controller-independent; the velocity/value column is the
// controller's colour/level scheme.
// ---------------------------------------------------------------------------
enum MidiFeedbackState {
    MFS_OFF = 0,
    MFS_LOOPER_EMPTY,
    MFS_LOOPER_RECORDING,
    MFS_LOOPER_PENDING,
    MFS_LOOPER_PLAY_SILENT,
    MFS_LOOPER_PLAY_LOW,      // clip peak > MFS_LOW threshold
    MFS_LOOPER_PLAY_MID,      // clip peak > MFS_MID threshold
    MFS_LOOPER_PLAY_HIGH,     // clip peak > MFS_HIGH threshold (clip light)
    MFS_LOOPER_PAUSED,        // has content, paused -> blink
    MFS_PRESET_UNUSED,
    MFS_PRESET_USED,
    MFS_LIVE_ENGAGE_ON,
    MFS_LIVE_ENGAGE_OFF,

    MFS_STATE_COUNT
};

struct MidiOutputMap {
    uint8_t state;         // MidiFeedbackState
    uint8_t statusType;    // MS_NOTE_ON (LED note) or MS_CC
    uint8_t velocity;      // colour/level value the controller expects
};

// ---------------------------------------------------------------------------
// A complete controller profile = the two atomic tables + their sizes + the
// peak thresholds the looper-VU states key off (also controller-tunable).
// ---------------------------------------------------------------------------
struct MidiControllerProfile {
    const char*           name;
    const MidiInputMap*   inputs;
    int                   numInputs;
    const MidiOutputMap*  outputs;
    int                   numOutputs;
    // VU thresholds for the LOOPER_PLAY_* states (clip-peak buckets).
    uint32_t              vuLow;     // > this -> at least PLAY_LOW
    uint32_t              vuMid;     // > this -> PLAY_MID
    uint32_t              vuHigh;    // > this -> PLAY_HIGH
    // CC1 mod-wheel deadzone (controller-specific neutral band).
    uint8_t               modDeadLo, modDeadHi;
};

// ---------------------------------------------------------------------------
// Resolver: find the atomic input row matching an event. Returns MA_NONE
// (a row whose action is MA_NONE) when nothing matches -> caller ignores it,
// exactly preserving the current "unknown event falls through" behavior.
// ---------------------------------------------------------------------------
static inline const MidiInputMap* midiMapResolveInput(
    const MidiControllerProfile* prof, uint8_t status, uint8_t data1)
{
    if (!prof) return 0;
    uint8_t st = status & 0xF0;
    int8_t  ch = (int8_t)(status & 0x0F);
    for (int i = 0; i < prof->numInputs; i++) {
        const MidiInputMap* m = &prof->inputs[i];
        if (m->statusType != st) continue;
        if (m->channel != MIDI_ANY_CHANNEL && m->channel != ch) continue;
        if (data1 < m->data1Lo || data1 > m->data1Hi) continue;
        return m;
    }
    return 0;
}

// Resolve a feedback state to its MIDI emission for this profile. Returns 0
// (no row) when the controller defines no output for that state -> the caller
// must treat that as "emit nothing" (graceful degrade, never a stuck LED).
static inline const MidiOutputMap* midiMapResolveOutput(
    const MidiControllerProfile* prof, uint8_t state)
{
    if (!prof) return 0;
    for (int i = 0; i < prof->numOutputs; i++)
        if (prof->outputs[i].state == state) return &prof->outputs[i];
    return 0;
}

// Decode a data2 byte per the row's value mode into a normalized result.
// Absolute -> the raw 0..127. Relative -> a signed delta. Trigger -> 1.
static inline int midiMapDecodeValue(uint8_t valueMode, uint8_t data2)
{
    switch (valueMode) {
        case MV_RELATIVE_TWOS:    return (int)data2 - 64;            // 65=+1, 63=-1
        case MV_RELATIVE_SIGNBIT: return (data2 & 0x40) ? -(int)(data2 & 0x3F)
                                                        :  (int)(data2 & 0x3F);
        case MV_TRIGGER:          return 1;
        default:                  return (int)data2;                // MV_ABSOLUTE
    }
}

// ===========================================================================
// DEFAULT PROFILE — APC Key 25.
// Every row below mirrors an existing hard-coded mapping. These are the values
// that make the firmware behave identically to the pre-config code; a
// different controller replaces this whole block.
// ===========================================================================

// APC physical addresses (the controller-specific part).
#define APC25_NOTE_PAD_LO   0x00   // grid pads occupy note 0 .. ROWS*COLS-1
#define APC25_NOTE_PAD_HI   0x27   // 5 rows * 8 cols - 1 = 39
#define APC25_BTN_STOP_ALL  0x51
#define APC25_BTN_PLAY      0x5B
#define APC25_BTN_RECORD    0x5D
#define APC25_BTN_FORMAT    0x54
#define APC25_BTN_SHIFT     0x62
#define APC25_LIVE_LED_NOTE 0x40   // live-engage LED feedback note

// APC LED colour velocities (the controller-specific colour scheme).
#define APC25_LED_OFF           0
#define APC25_LED_GREEN         1
#define APC25_LED_GREEN_BLINK   2
#define APC25_LED_RED           3
#define APC25_LED_RED_BLINK     4
#define APC25_LED_YELLOW        5
#define APC25_LED_YELLOW_BLINK  6

static const MidiInputMap g_apc25Inputs[] = {
    // --- modifier (must precede pad/button so SHIFT note isn't a pad) ---
    { MS_NOTE_ON,  MIDI_ANY_CHANNEL, APC25_BTN_SHIFT,  APC25_BTN_SHIFT,  MV_TRIGGER,  MA_SHIFT,               0  },

    // --- live-pitch notes by channel (APC multi-channel nuance) ---
    { MS_NOTE_ON,  0,  64, 64, MV_TRIGGER,  MA_BTN_TOGGLE_LIVE,     0 },   // ch0 note64 toggle
    { MS_NOTE_ON,  1,   0,127, MV_ABSOLUTE, MA_LIVE_PITCH_NOTE_CH1, -1 },  // ch1 note=pitch, engage
    { MS_NOTE_ON,  2,   0,127, MV_ABSOLUTE, MA_LIVE_PITCH_NOTE_CH2, -1 },  // ch2 note=pitch, always engage

    // --- grid pads + global transport buttons (channel-agnostic note range) ---
    { MS_NOTE_ON,  MIDI_ANY_CHANNEL, APC25_NOTE_PAD_LO, APC25_NOTE_PAD_HI, MV_TRIGGER, MA_PAD,          -1 },
    { MS_NOTE_ON,  MIDI_ANY_CHANNEL, APC25_BTN_STOP_ALL, APC25_BTN_STOP_ALL, MV_TRIGGER, MA_BTN_STOP_ALL, 0 },
    { MS_NOTE_ON,  MIDI_ANY_CHANNEL, APC25_BTN_RECORD,  APC25_BTN_RECORD,  MV_TRIGGER, MA_BTN_RECORD,    0 },
    { MS_NOTE_ON,  MIDI_ANY_CHANNEL, APC25_BTN_PLAY,    APC25_BTN_PLAY,    MV_TRIGGER, MA_BTN_PLAY,      0 },
    { MS_NOTE_ON,  MIDI_ANY_CHANNEL, APC25_BTN_FORMAT,  APC25_BTN_FORMAT,  MV_TRIGGER, MA_BTN_TOGGLE_LIVE, 0 },

    // --- continuous controllers ---
    { MS_CC, MIDI_ANY_CHANNEL,   1,   1, MV_ABSOLUTE, MA_LIVE_PITCH_MODWHEEL, 0 },   // CC1 mod wheel
    { MS_CC, MIDI_ANY_CHANNEL,  52,  52, MV_ABSOLUTE, MA_LIVE_PITCH_CC,       0 },   // CC52 linear pitch
    { MS_CC, MIDI_ANY_CHANNEL,  51,  51, MV_ABSOLUTE, MA_FILTER_CC,          51 },   // HP
    { MS_CC, MIDI_ANY_CHANNEL,  54,  54, MV_ABSOLUTE, MA_FILTER_CC,          54 },   // resonance
    { MS_CC, MIDI_ANY_CHANNEL,  55,  55, MV_ABSOLUTE, MA_FILTER_CC,          55 },   // LP
    { MS_CC, MIDI_ANY_CHANNEL,  48,  48, MV_ABSOLUTE, MA_EFFECT_CC,          48 },   // reverb
    { MS_CC, MIDI_ANY_CHANNEL,  49,  49, MV_ABSOLUTE, MA_EFFECT_CC,          49 },   // delay
    { MS_CC, MIDI_ANY_CHANNEL,  50,  50, MV_ABSOLUTE, MA_EFFECT_CC,          50 },   // time
    { MS_CC, MIDI_ANY_CHANNEL,  53,  53, MV_ABSOLUTE, MA_EFFECT_CC,          53 },   // formant depth
    { MS_CC, MIDI_ANY_CHANNEL,  56,  56, MV_ABSOLUTE, MA_EFFECT_CC,          56 },   // (unmapped audio, handler no-op)
    { MS_CC, MIDI_ANY_CHANNEL,  57,  57, MV_ABSOLUTE, MA_EFFECT_CC,          57 },   // (unmapped audio, handler no-op)
    // engine-tuning CCs (UDP inject only)
    { MS_CC, MIDI_ANY_CHANNEL, 100, 107, MV_ABSOLUTE, MA_ENGINE_TUNE_CC,    -1 },
};

static const MidiOutputMap g_apc25Outputs[] = {
    { MFS_OFF,                MS_NOTE_ON, APC25_LED_OFF          },
    { MFS_LOOPER_EMPTY,       MS_NOTE_ON, APC25_LED_OFF          },
    { MFS_LOOPER_RECORDING,   MS_NOTE_ON, APC25_LED_RED_BLINK    },
    { MFS_LOOPER_PENDING,     MS_NOTE_ON, APC25_LED_YELLOW       },
    { MFS_LOOPER_PLAY_SILENT, MS_NOTE_ON, APC25_LED_GREEN        },
    { MFS_LOOPER_PLAY_LOW,    MS_NOTE_ON, APC25_LED_GREEN        },
    { MFS_LOOPER_PLAY_MID,    MS_NOTE_ON, APC25_LED_YELLOW       },
    { MFS_LOOPER_PLAY_HIGH,   MS_NOTE_ON, APC25_LED_RED          },
    { MFS_LOOPER_PAUSED,      MS_NOTE_ON, APC25_LED_YELLOW_BLINK },
    { MFS_PRESET_UNUSED,      MS_NOTE_ON, APC25_LED_OFF          },
    { MFS_PRESET_USED,        MS_NOTE_ON, APC25_LED_YELLOW       },
    { MFS_LIVE_ENGAGE_ON,     MS_NOTE_ON, 127                    },
    { MFS_LIVE_ENGAGE_OFF,    MS_NOTE_ON, 0                      },
};

static const MidiControllerProfile g_apc25Profile = {
    "APC Key 25",
    g_apc25Inputs,  (int)(sizeof(g_apc25Inputs)  / sizeof(g_apc25Inputs[0])),
    g_apc25Outputs, (int)(sizeof(g_apc25Outputs) / sizeof(g_apc25Outputs[0])),
    /*vuLow=*/  200,   // matches _updateGridLeds: >1500 mid, >8000 high; low band is "playing+audio"
    /*vuMid=*/  1500,
    /*vuHigh=*/ 8000,
    /*modDeadLo=*/ 59, /*modDeadHi=*/ 69
};

// The active profile pointer — swap this (or load from SD) to drive a
// different controller without changing any caller.
static const MidiControllerProfile* g_activeProfile = &g_apc25Profile;

#endif // MIDIMAP_H
