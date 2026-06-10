// MIDI-config parity: the atomic midiMap.h APC25 profile must resolve EVERY
// MIDI event the APC Key 25 emits to the SAME logical action / LED emission
// the existing hard-coded code takes — proving the config extraction does not
// change any operational control (the load-bearing no-operational-change
// invariant from the request: "we must not change any of the operational
// controls in the process").
//
// Strategy: a `legacy*` function replicates the exact branch structure of
// apcKey25::handleMidi / _onButton / handleFilterCC / handleEffectsCC and
// _updateGridLeds (the current hard-coded dispatch). Then for the full input
// event matrix we assert midiMapResolveInput()+midiMapDecodeValue() classify
// each event the same way legacy does, and for every feedback state we assert
// midiMapResolveOutput() returns the same velocity legacy _updateGridLeds
// would send.
//
// Build: g++ -O2 -std=c++17 scripts/test-midi-config-parity.cpp -o scripts/test-midi-config-parity.exe
#include <cstdio>
#include <cstdint>
#include "../midiMap.h"

static int g_fails = 0;
static void check(const char* n, bool c){ if(!c){printf("FAIL: %s\n",n);g_fails++;} }

// ---- legacy classification: returns a stable tag string for an input event,
//      mirroring the branch the real handleMidi/_onButton would take. ----
static const char* legacyInputTag(uint8_t status, uint8_t data1, uint8_t data2)
{
    uint8_t st = status & 0xF0;
    uint8_t ch = status & 0x0F;

    if (st == 0x90 && data2 > 0) {                       // note-on
        if (data1 == 0x62) return "SHIFT";
        if (ch == 0 && data1 == 64) return "TOGGLE_LIVE";
        if (ch == 1) return "PITCH_CH1";
        if (ch == 2) return "PITCH_CH2";
        // microrepeat latch notes 82-86 checked BEFORE pad/button (note 84
        // overrides FORMAT) - matches the new handleMidi order.
        if (data1 >= 82 && data1 <= 86) return "MICROREPEAT";
        // sampler control buttons (ch0, free track buttons 65/66) checked before
        // pad/button dispatch - matches the new handleMidi order.
        if (ch == 0 && data1 == 65) return "SAMPLER_RECORD";
        if (ch == 0 && data1 == 66) return "SAMPLER_DRUM_MODE";
        if (data1 < 40)  return "PAD";                   // APC_ROWS*APC_COLS=40
        if (data1 == 0x51) return "STOP_ALL";
        if (data1 == 0x5D) return "RECORD";
        if (data1 == 0x5B) return "PLAY";
        if (data1 == 0x54) return "TOGGLE_LIVE";         // FORMAT button
        return "NONE";
    }
    if (st == 0xB0) {                                     // CC
        if (data1 == 1)  return "MODWHEEL";
        if (data1 == 52) return "PITCH_CC";
        if (data1 == 51 || data1 == 54 || data1 == 55) return "FILTER";
        if (data1 == 48 || data1 == 49 || data1 == 50 ||
            data1 == 53 || data1 == 56 || data1 == 57) return "EFFECT";
        if (data1 >= 100 && data1 <= 107) return "ENGINE_TUNE";
        return "NONE";
    }
    return "NONE";                                       // note-off etc not classified here
}

// Map a resolved MidiAction back to the same tag vocabulary.
static const char* actionTag(uint8_t action)
{
    switch (action) {
        case MA_SHIFT:               return "SHIFT";
        case MA_BTN_TOGGLE_LIVE:     return "TOGGLE_LIVE";
        case MA_LIVE_PITCH_NOTE_CH1: return "PITCH_CH1";
        case MA_LIVE_PITCH_NOTE_CH2: return "PITCH_CH2";
        case MA_PAD:                 return "PAD";
        case MA_BTN_STOP_ALL:        return "STOP_ALL";
        case MA_BTN_RECORD:          return "RECORD";
        case MA_BTN_PLAY:            return "PLAY";
        case MA_LIVE_PITCH_MODWHEEL: return "MODWHEEL";
        case MA_LIVE_PITCH_CC:       return "PITCH_CC";
        case MA_FILTER_CC:           return "FILTER";
        case MA_EFFECT_CC:           return "EFFECT";
        case MA_ENGINE_TUNE_CC:      return "ENGINE_TUNE";
        case MA_MICROREPEAT:         return "MICROREPEAT";
        case MA_SAMPLER_RECORD:      return "SAMPLER_RECORD";
        case MA_SAMPLER_DRUM_MODE:   return "SAMPLER_DRUM_MODE";
        case MA_SAMPLER_KEY:         return "SAMPLER_KEY";
        default:                     return "NONE";
    }
}

static const char* resolvedInputTag(uint8_t status, uint8_t data1)
{
    const MidiInputMap* m = midiMapResolveInput(g_activeProfile, status, data1);
    return m ? actionTag(m->action) : "NONE";
}

#include <cstring>
static bool tageq(const char* a, const char* b){ return std::strcmp(a,b)==0; }

// ---- legacy _updateGridLeds LED velocity for a looper VU/state ----
static uint8_t legacyLooperVel(bool recording, bool pending, bool playing,
                               uint32_t cpeak, bool hasContent)
{
    if (recording) return APC25_LED_RED_BLINK;
    if (pending)   return APC25_LED_YELLOW;
    if (playing) {
        if      (cpeak > 8000) return APC25_LED_RED;
        else if (cpeak > 1500) return APC25_LED_YELLOW;
        else                   return APC25_LED_GREEN;
    }
    if (hasContent) return APC25_LED_YELLOW_BLINK;
    return APC25_LED_OFF;
}

// resolve a feedback state to a velocity via the config (graceful if missing).
static int cfgVel(uint8_t state)
{
    const MidiOutputMap* o = midiMapResolveOutput(g_activeProfile, state);
    return o ? (int)o->velocity : -1;
}

int main()
{
    // ===== INPUT PARITY: full event matrix =====
    // note-on across all 16 channels, every data1, velocity 1 and 127.
    for (int ch = 0; ch < 16; ch++) {
        uint8_t status = (uint8_t)(0x90 | ch);
        for (int d1 = 0; d1 < 128; d1++) {
            const char* leg = legacyInputTag(status, (uint8_t)d1, 100);
            const char* cfg = resolvedInputTag(status, (uint8_t)d1);
            if (!tageq(leg, cfg)) {
                printf("FAIL note-on ch%d d1=%d legacy=%s cfg=%s\n", ch, d1, leg, cfg);
                g_fails++;
            }
        }
    }
    // CC across all channels and CC numbers.
    for (int ch = 0; ch < 16; ch++) {
        uint8_t status = (uint8_t)(0xB0 | ch);
        for (int d1 = 0; d1 < 128; d1++) {
            const char* leg = legacyInputTag(status, (uint8_t)d1, 64);
            const char* cfg = resolvedInputTag(status, (uint8_t)d1);
            if (!tageq(leg, cfg)) {
                printf("FAIL cc ch%d d1=%d legacy=%s cfg=%s\n", ch, d1, leg, cfg);
                g_fails++;
            }
        }
    }
    printf("input matrix parity checked (note-on + CC, all channels)\n");

    // ===== OUTPUT PARITY: looper VU/state -> velocity =====
    check("rec=red_blink",     cfgVel(MFS_LOOPER_RECORDING)   == APC25_LED_RED_BLINK);
    check("pending=yellow",    cfgVel(MFS_LOOPER_PENDING)     == APC25_LED_YELLOW);
    check("play_high=red",     cfgVel(MFS_LOOPER_PLAY_HIGH)   == APC25_LED_RED);
    check("play_mid=yellow",   cfgVel(MFS_LOOPER_PLAY_MID)    == APC25_LED_YELLOW);
    check("play_low=green",    cfgVel(MFS_LOOPER_PLAY_LOW)    == APC25_LED_GREEN);
    check("play_silent=green", cfgVel(MFS_LOOPER_PLAY_SILENT) == APC25_LED_GREEN);
    check("paused=yellowblink",cfgVel(MFS_LOOPER_PAUSED)      == APC25_LED_YELLOW_BLINK);
    check("empty=off",         cfgVel(MFS_LOOPER_EMPTY)       == APC25_LED_OFF);
    check("preset_used=yellow",cfgVel(MFS_PRESET_USED)        == APC25_LED_YELLOW);
    check("preset_unused=off", cfgVel(MFS_PRESET_UNUSED)      == APC25_LED_OFF);
    check("live_on=127",       cfgVel(MFS_LIVE_ENGAGE_ON)     == 127);
    check("live_off=0",        cfgVel(MFS_LIVE_ENGAGE_OFF)    == 0);

    // VU-bucket -> state -> velocity must equal legacy _updateGridLeds path.
    struct { uint32_t peak; } cases[] = {{0},{200},{1500},{1501},{8000},{8001},{50000}};
    for (auto& c : cases) {
        uint8_t leg = legacyLooperVel(false,false,true, c.peak, true);
        uint8_t state = (c.peak > g_activeProfile->vuHigh) ? MFS_LOOPER_PLAY_HIGH
                      : (c.peak > g_activeProfile->vuMid)  ? MFS_LOOPER_PLAY_MID
                                                           : MFS_LOOPER_PLAY_LOW;
        // note: legacy GREEN covers both "low audio" and "silent" -> both map to GREEN
        int cfg = cfgVel(state);
        if (cfg != (int)leg) { printf("FAIL vu peak=%u legacy=%d cfg=%d\n", c.peak, leg, cfg); g_fails++; }
    }

    // ===== EDGE: unmapped input ignored cleanly =====
    check("unmapped note-on(70) -> NONE", tageq(resolvedInputTag(0x90, 70), "NONE"));
    check("unmapped CC(10) -> NONE",      tageq(resolvedInputTag(0xB0, 10), "NONE"));

    // ===== EDGE: relative / endless encoder decode (controller nuance) =====
    check("abs decode passes 100",   midiMapDecodeValue(MV_ABSOLUTE, 100) == 100);
    check("twos +1",                 midiMapDecodeValue(MV_RELATIVE_TWOS, 65) == 1);
    check("twos -1",                 midiMapDecodeValue(MV_RELATIVE_TWOS, 63) == -1);
    check("signbit +5",              midiMapDecodeValue(MV_RELATIVE_SIGNBIT, 5) == 5);
    check("signbit -5",              midiMapDecodeValue(MV_RELATIVE_SIGNBIT, 0x40|5) == -5);
    check("trigger -> 1",            midiMapDecodeValue(MV_TRIGGER, 0) == 1);

    // ===== EDGE: partial output profile degrades to silence, not crash =====
    {
        static const MidiOutputMap tiny[] = { { MFS_LOOPER_RECORDING, MS_NOTE_ON, 3 } };
        static const MidiControllerProfile minimal = {
            "minimal", g_apc25Inputs, 1, tiny, 1, 200,1500,8000, 59,69 };
        check("partial profile: defined state resolves",
              midiMapResolveOutput(&minimal, MFS_LOOPER_RECORDING) != 0);
        check("partial profile: missing state -> null (silent, no crash)",
              midiMapResolveOutput(&minimal, MFS_LOOPER_PAUSED) == 0);
    }

    // ===== EDGE: null profile is safe =====
    check("null profile input -> null",  midiMapResolveInput(0, 0x90, 5) == 0);
    check("null profile output -> null", midiMapResolveOutput(0, MFS_OFF) == 0);

    if (g_fails) { printf("\n%d FAIL\n", g_fails); return 1; }
    printf("\nALL PASS\n");
    return 0;
}
