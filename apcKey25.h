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
#define APC_BTN_FORMAT      0x54
#define APC_BTN_SHIFT       0x62

#define APC_CH_NOTE_ON      0x90
#define APC_CH_NOTE_OFF     0x80

#define APC_VEL_LED_OFF            0
#define APC_VEL_LED_GREEN          1
#define APC_VEL_LED_GREEN_BLINK    2
#define APC_VEL_LED_RED            3
#define APC_VEL_LED_RED_BLINK      4
#define APC_VEL_LED_YELLOW         5
#define APC_VEL_LED_YELLOW_BLINK   6

#define APC_HOLD_ERASE_MS   1000
#define APC_LED_BOOT_DELAY_MS 2000

// We map looper tracks to APC rows from bottom: track 0 = row 0 (bottom row)
// Col 0 = rec/play/stop track button
// Col 1 = track presence (tap to erase, hold to erase)

struct ApcCmd {
    enum Type { NONE, TRACK, STOP_TRACK, LOOPER, CLEAR_LAYER, PRESET_RESTORE } type;
    int arg;
};

class apcKey25
{
public:
    apcKey25();

    void handleMidi(u8 status, u8 data1, u8 data2);
    void handleFilterCC(u8 cc, u8 data2);
    void handleEffectsCC(u8 cc, u8 data2);
    void update();
    void invalidateLedCache();

    struct DebugState {
        bool transposeLocked;
        int transposePitch;
        float pitchWheelOffset;
        float driftTarget;
        float computedRatio;
        bool liveEngaged;
        float livePitchSemitones;
    };

    struct EffectsState {
        float filterHP;
        float filterLP;
        float filterRes;
        float reverbAmount;
        float delayAmount;
        float time;
        float formant;
    };

    DebugState getDebugState() const;
    EffectsState getEffectsState() const;

private:
    bool          m_shift;
    // Lock-free SPSC command ring: producer = MIDI ISR (_queueCmd via
    // handleMidi/note handlers), consumer = Core-2 update(). Was a single
    // slot (m_cmdReady/Type/Arg) that silently OVERWROTE a queued command if a
    // second pad event arrived before update() drained it — dropping presses
    // and adding jitter. A ring keeps every press in order so each looper
    // responds promptly and identically.
    static const int APC_CMD_RING = 32;
    // press_ticks = CTimer microseconds captured at the MIDI ISR (handleMidi),
    // the EARLIEST observable moment of the press. Carried through the ring so
    // the record-start/stop can backdate to the true press instant regardless
    // of how long the press waited for the Core-2 drain. 0 = no timestamp.
    struct ApcCmdSlot { ApcCmd::Type type; int arg; unsigned press_ticks; };
    volatile ApcCmdSlot m_cmdRing[APC_CMD_RING];
    volatile unsigned   m_cmdHead;   // producer writes, consumer reads
    volatile unsigned   m_cmdTail;   // consumer advances

    // Per-pad hold tracking for the 20 looper pads (cols 2-5, rows 0-4).
    // Index = row*4 + (col-2) ∈ [0,20). Tap = press cycle (rec/play/pause).
    // Long-hold (>= APC_HOLD_ERASE_MS) = clear that looper.
    unsigned long m_looperHoldStart[LOOPER_NUM_TRACKS];
    bool          m_looperHeld[LOOPER_NUM_TRACKS];
    bool          m_looperClearTriggered[LOOPER_NUM_TRACKS];
    bool          m_looperRecordedOnPress[LOOPER_NUM_TRACKS];  // armed rec on press; suppress release tap

    // Per-pad hold tracking for the 10 preset pads (cols 0-1, rows 0-4).
    // Index = row*2 + col ∈ [0,10). Tap = restore preset (mute all non-set
    // loopers, unmute set loopers). Long-hold = capture: snapshot which
    // loopers are currently playing into this preset slot.
    unsigned long m_presetHoldStart[LOOPER_NUM_PRESETS];
    bool          m_presetHeld[LOOPER_NUM_PRESETS];
    bool          m_presetCaptured[LOOPER_NUM_PRESETS];

    // Stored presets: each is a bit-mask of which loopers should be playing.
    // bit n = looper n plays when this preset is recalled.
    u32           m_presetMask[LOOPER_NUM_PRESETS];
    bool          m_presetUsed[LOOPER_NUM_PRESETS];

    unsigned long m_nowMs;
    unsigned long m_bootMs;
    unsigned long m_lastLedMs;

    bool          m_transposeLocked;
    int           m_transposePitch;
    int           m_pitchWheelOffset;
    float         m_driftTarget;
    unsigned long m_lastDriftMs;
    float         m_computedRatio;
    bool          m_liveEngaged;
    float         m_livePitchSemitones;
    volatile bool m_liveLedDirty;

    float         m_filterHP;
    float         m_filterLP;
    float         m_filterRes;

    float         m_reverbAmount;
    float         m_delayAmount;
    float         m_time;
    float         m_formant;            // brightness, -1..+1 (CC53)
    float         m_formantResonance;    // 0..1 peaking gain (CC56)
    float         m_formantFreq;         // 300..3000 Hz peak center (CC57)

    void _applyLivePitch();
    void _applyFilters();
    void _applyEffects();
    void _applyFormant();
    void _capturePreset(int p);
    void _applyPreset(int p);
    // Arrangement memory upkeep: when a looper is erased/cleared-to-empty,
    // drop its bit from every stored preset mask. Any preset whose mask
    // becomes empty is deleted (m_presetUsed=false) so its LED goes dark.
    void _forgetLooperFromPresets(int n);
    // Whole-bank clear: forget every looper from every preset and delete
    // all now-empty arrangements (CLEAR_ALL / STOP-ALL-erase paths).
    void _forgetAllPresets();
    int  _looperFromPad(int row, int col) const;  // returns 0..19 or -1
    int  _presetFromPad(int row, int col) const;  // returns 0..9 or -1
    void _queueCmd(ApcCmd::Type type, int arg);
    void _onPadPress(int row, int col);
    void _onPadRelease(int row, int col);
    void _onButton(u8 note);
    void _sendLed(u8 note, u8 velocity);
    void _updateComputedRatio();
    void _updateDrift();
    void _updateGridLeds();
    u8   _padNote(int row, int col);
};

extern apcKey25 *pTheAPC;

#endif
