#define log_name "apc25"

#include "apcKey25.h"
#include "input_usb.h"
#include "usbMidi.h"
#include "abletonLink.h"
#include "patches/paramSnapshot.h"
#include "patches/RubberBandWrapper.h"
#include <circle/logger.h>
#include <circle/timer.h>

extern RubberBandWrapper *pLivePitchWrapper;

apcKey25 *pTheAPC = 0;

apcKey25::apcKey25()
    : m_shift(false), m_cmdHead(0), m_cmdTail(0),
      m_nowMs(0), m_bootMs(0), m_lastLedMs(0),
      m_transposeLocked(false), m_transposePitch(0), m_pitchWheelOffset(0),
      m_driftTarget(0.0f), m_lastDriftMs(0), m_computedRatio(1.0f),
      m_liveEngaged(false), m_livePitchSemitones(0.0f), m_liveLedDirty(false),
      m_filterHP(0.0f), m_filterLP(1.0f), m_filterRes(0.0f),
      m_reverbAmount(0.0f), m_delayAmount(0.0f), m_time(0.5f), m_formant(0.0f),
      m_formantResonance(0.0f), m_formantFreq(800.0f)
{
    pTheAPC = this;
    // Boot with formant at CENTER (0) = pure continuous-reader -12, the CLEAN
    // non-grainy path. Off-center crossfades toward the grain path (grainier
    // but formant-shiftable). User confirmed the grain path sounds grainy, so
    // center is the default good -12.
    for (int i = 0; i < LOOPER_NUM_TRACKS; i++)
    {
        m_looperHoldStart[i]       = 0;
        m_looperHeld[i]            = false;
        m_looperClearTriggered[i]  = false;
        m_looperRecordedOnPress[i] = false;
    }
    for (int i = 0; i < LOOPER_NUM_PRESETS; i++)
    {
        m_presetHoldStart[i]   = 0;
        m_presetHeld[i]        = false;
        m_presetCaptured[i]    = false;
        m_presetMask[i]        = 0;
        m_presetUsed[i]        = false;
    }
}

u8 apcKey25::_padNote(int row, int col)
{
    return (u8)(row * APC_COLS + col);
}

void apcKey25::_sendLed(u8 note, u8 velocity)
{
    usbMidiSendNoteOn(note, velocity);
}

void apcKey25::_queueCmd(ApcCmd::Type type, int arg)
{
    // SPSC ring push (producer = MIDI ISR). Drop only if full (32 unread) —
    // never overwrite an unread command. head==tail means empty.
    unsigned head = m_cmdHead;
    unsigned next = (head + 1) % APC_CMD_RING;
    if (next == m_cmdTail) return;            // ring full — drop (should never happen at tap rates)
    m_cmdRing[head].type = type;
    m_cmdRing[head].arg  = arg;
    m_cmdHead = next;                          // publish after fields written
}

void apcKey25::handleMidi(u8 status, u8 data1, u8 data2)
{
    u8 msgType = status & 0xF0;
    u8 channel = status & 0x0F;

    if (msgType == APC_CH_NOTE_ON && data2 > 0)
    {
        if (data1 == APC_BTN_SHIFT) { m_shift = true; return; }
        if (channel == 0 && data1 == 64) {
            m_liveEngaged = !m_liveEngaged;
            if (!m_liveEngaged) m_livePitchSemitones = 0.0f;
            _applyLivePitch();
            return;
        }
        if (channel == 1) {
            m_livePitchSemitones = (float)((int)data1 - 60);
            m_liveEngaged = true;
            _applyLivePitch();
            return;
        }
        if (channel == 2) {
            // 0x92 (MIDI ch 3): always-engaged pitch set from note.
            // Used by UDP-MIDI inject from host harness for repeatable
            // engage-at-pitch testing. Per AGENTS.md spec.
            m_livePitchSemitones = (float)((int)data1 - 60);
            m_liveEngaged = true;
            _applyLivePitch();
            return;
        }
        if (data1 < APC_ROWS * APC_COLS)
        {
            _onPadPress(data1 / APC_COLS, data1 % APC_COLS);
            return;
        }
        _onButton(data1);
        return;
    }

    if (msgType == APC_CH_NOTE_OFF || (msgType == APC_CH_NOTE_ON && data2 == 0))
    {
        if (data1 == APC_BTN_SHIFT) { m_shift = false; return; }
        if (channel == 0 && data1 == 64) return;
        if (channel == 1) { m_transposeLocked = false; return; }
        if (data1 < APC_ROWS * APC_COLS)
            _onPadRelease(data1 / APC_COLS, data1 % APC_COLS);
        return;
    }

    if (msgType == 0xB0 && data1 == 1)
    {
        bool inDeadzone = (data2 >= 59 && data2 <= 69);
        if (inDeadzone) {
            m_liveEngaged = false;
            m_livePitchSemitones = 0.0f;
        } else {
            m_livePitchSemitones = ((float)((int)data2 - 64)) * 12.0f / 63.0f;
            m_liveEngaged = true;
        }
        _applyLivePitch();
        return;
    }

    if (msgType == 0xB0 && data1 == 52)
    {
        m_livePitchSemitones = (data2 / 127.0f) * 24.0f - 12.0f;
        m_liveEngaged = true;
        _applyLivePitch();
        return;
    }

    if (msgType == 0xB0 && (data1 == 51 || data1 == 54 || data1 == 55))
    {
        handleFilterCC(data1, data2);
        return;
    }

    if (msgType == 0xB0 && (data1 == 48 || data1 == 49 || data1 == 50 || data1 == 53 || data1 == 56 || data1 == 57))
    {
        handleEffectsCC(data1, data2);
        return;
    }

    // Live engine-tuning CCs (UDP-MIDI inject only — no APC knob assigned).
    // Use scripts/tune-engine.ps1 or the 3-loopback sweep harness to send
    // these and observe results in real-time without rebuilding.
    if (msgType == 0xB0 && data1 == 100)
    {
        // CC100: engine initial read offset (= algorithmic latency).
        // data2 0..127 maps log-ish to 32..2048 samples (0.66..43 ms @ 48k).
        // Default 192 ≈ data2=48.
        int samp = 32 + (int)((float)data2 * (float)data2 * 0.125f);
        if (samp > 2048) samp = 2048;
        if (pLivePitchWrapper) pLivePitchWrapper->setEngineReadOffset(samp);
        CLogger::Get()->Write(log_name, LogNotice, "tune readOffset=%d", samp);
        return;
    }
    if (msgType == 0xB0 && data1 == 101)
    {
        // CC101: splice xfade scale. data2 0..127 → 0.25..4.0 (log).
        // Default 1.0 ≈ data2=64.
        float s = 0.25f * powf(16.0f, (float)data2 / 127.0f);
        if (pLivePitchWrapper) pLivePitchWrapper->setEngineXfadeScale(s);
        CLogger::Get()->Write(log_name, LogNotice, "tune xfadeScale=%.3f", s);
        return;
    }
    if (msgType == 0xB0 && data1 == 102)
    {
        // CC102: SNAC fidelity threshold. data2 0..127 → 0.30..0.95 linear.
        // Default 0.7 ≈ data2=88.
        float f = 0.30f + ((float)data2 / 127.0f) * 0.65f;
        if (pLivePitchWrapper) pLivePitchWrapper->setEngineFidelity(f);
        CLogger::Get()->Write(log_name, LogNotice, "tune fidelity=%.3f", f);
        return;
    }
    if (msgType == 0xB0 && data1 == 103)
    {
        // CC103: pre-resample bypass. data2 < 64 = off (engine runs normally),
        // data2 >= 64 = bypass. When formantDepth=0 the bypass is auto-applied
        // anyway, but the toggle lets us A/B the stage on/off explicitly.
        bool on = (data2 >= 64);
        if (pLivePitchWrapper) pLivePitchWrapper->setEnginePreBypass(on);
        CLogger::Get()->Write(log_name, LogNotice, "tune preBypass=%d", on?1:0);
        return;
    }
    if (msgType == 0xB0 && data1 == 104)
    {
        // CC104: splice integer-period snap. data2 < 64 = off, >= 64 = on.
        bool on = (data2 >= 64);
        if (pLivePitchWrapper) pLivePitchWrapper->setEngineSpliceSnap(on);
        CLogger::Get()->Write(log_name, LogNotice, "tune spliceSnap=%d", on?1:0);
        return;
    }
    if (msgType == 0xB0 && data1 == 105)
    {
        // CC105: splice value-match refinement. data2 < 64 = off, >= 64 = on.
        bool on = (data2 >= 64);
        if (pLivePitchWrapper) pLivePitchWrapper->setEngineSpliceMatch(on);
        CLogger::Get()->Write(log_name, LogNotice, "tune spliceMatch=%d", on?1:0);
        return;
    }
    if (msgType == 0xB0 && data1 == 106)
    {
        // CC106: drift low band. data2 0..127 → 1..32 samples (linear).
        int s = 1 + (int)(((float)data2 / 127.0f) * 31.0f);
        if (pLivePitchWrapper) pLivePitchWrapper->setEngineDriftLow(s);
        CLogger::Get()->Write(log_name, LogNotice, "tune driftLow=%d", s);
        return;
    }
    if (msgType == 0xB0 && data1 == 107)
    {
        // CC107: drift high head. data2 0..127 → 16..1024 samples (linear).
        int s = 16 + (int)(((float)data2 / 127.0f) * 1008.0f);
        if (pLivePitchWrapper) pLivePitchWrapper->setEngineDriftHigh(s);
        CLogger::Get()->Write(log_name, LogNotice, "tune driftHigh=%d", s);
        return;
    }
}

void apcKey25::update()
{
    if (!pTheLooper) return;

    // Re-publish snapshot every tick so Core 1 DSP sees fresh link tempo
    // and any externally-mutated state without depending on MIDI handlers.
    {
        LiveParams p;
        p.liveEngaged        = m_liveEngaged;
        p.livePitchSemitones = m_livePitchSemitones;
        p.formantNorm        = m_formant;
        p.linkSynced         = linkIsSynced();
        p.linkBPM            = (float)linkGetBPM();
        p.masterLoopBlocks   = pTheLooper->m_masterLoopBlocks;
        paramSnapshotPublish(p);
    }

    if (m_liveLedDirty)
    {
        m_liveLedDirty = false;
        usbMidiSend(0x90, 0x40, m_liveEngaged ? 127 : 0);
    }

    m_nowMs = CTimer::Get()->GetClockTicks() / 1000;

    if (m_bootMs == 0) m_bootMs = m_nowMs;

    // Drain ALL queued commands this tick (in press order) so nothing waits an
    // extra tick or gets dropped — keeps every looper's response prompt + equal.
    while (m_cmdTail != m_cmdHead)
    {
        ApcCmd::Type type = m_cmdRing[m_cmdTail].type;
        int arg = m_cmdRing[m_cmdTail].arg;
        m_cmdTail = (m_cmdTail + 1) % APC_CMD_RING;

        if (type == ApcCmd::TRACK)
        {
            pTheLooper->command(LOOP_COMMAND_TRACK_BASE + arg);
        }
        else if (type == ApcCmd::STOP_TRACK)
        {
            pTheLooper->command(LOOP_COMMAND_STOP_TRACK_BASE + arg);
        }
        else if (type == ApcCmd::CLEAR_LAYER)
        {
            pTheLooper->command(LOOP_COMMAND_CLEAR_LAYER_BASE + arg);
        }
        else if (type == ApcCmd::PRESET_RESTORE)
        {
            _applyPreset(arg);
        }
        else if (type == ApcCmd::LOOPER)
        {
            pTheLooper->command(arg);
        }
    }
    _updateDrift();
    // Per-looper long-hold → clear-layer (also clears recording state).
    for (int n = 0; n < LOOPER_NUM_TRACKS; n++)
    {
        if (m_looperHeld[n] && !m_looperClearTriggered[n])
        {
            if (m_nowMs - m_looperHoldStart[n] >= APC_HOLD_ERASE_MS)
            {
                m_looperClearTriggered[n] = true;
                m_looperHeld[n] = false;
                pTheLooper->command(LOOP_COMMAND_CLEAR_LAYER_BASE + n);
            }
        }
    }
    // Per-preset long-hold → capture current play-state mask.
    for (int p = 0; p < LOOPER_NUM_PRESETS; p++)
    {
        if (m_presetHeld[p] && !m_presetCaptured[p])
        {
            if (m_nowMs - m_presetHoldStart[p] >= APC_HOLD_ERASE_MS)
            {
                m_presetCaptured[p] = true;
                m_presetHeld[p] = false;
                _capturePreset(p);
            }
        }
    }
    if (m_nowMs - m_bootMs < APC_LED_BOOT_DELAY_MS) return;
    if (m_nowMs - m_lastLedMs < 33) return;
    m_lastLedMs = m_nowMs;
    _updateGridLeds();
}