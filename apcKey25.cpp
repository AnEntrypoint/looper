#define log_name "apc25"

#include "apcKey25.h"
#include "input_usb.h"
#include "usbMidi.h"
#include "abletonLink.h"
#include "midiMap.h"
// Only the press-ticks publish hook is needed here; avoid pulling the full
// continuousBuffer.h (which needs Looper.h macros) into this translation unit.
extern volatile unsigned g_pendingPressTicks;
#include "patches/paramSnapshot.h"
#include "patches/RubberBandWrapper.h"
#include "patches/sampler.h"
#include <circle/logger.h>
#include <circle/timer.h>

extern RubberBandWrapper *pLivePitchWrapper;
extern sampler *pSampler;
// Drum-record mode witness for the :4445 TIME verb (defined in loopMachine.cpp).
extern volatile u32 g_samplerDrumMode;

apcKey25 *pTheAPC = 0;

apcKey25::apcKey25()
    : m_shift(false), m_microRepeatDiv(0), m_drumRecordMode(false),
      m_cmdHead(0), m_cmdTail(0),
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
    // Stamp press_ticks HERE: _queueCmd runs inside handleMidi (the MIDI IN
    // ISR), so this is the earliest observable moment of the press. One timer
    // read, no alloc — ISR-safe (same CTimer::GetClockTicks already used by
    // input_usb g_inLastTicks). Backdating uses this as the true press instant.
    unsigned head = m_cmdHead;
    unsigned next = (head + 1) % APC_CMD_RING;
    if (next == m_cmdTail) return;            // ring full — drop (should never happen at tap rates)
    m_cmdRing[head].type = type;
    m_cmdRing[head].arg  = arg;
    m_cmdRing[head].press_ticks = CTimer::GetClockTicks();
    m_cmdHead = next;                          // publish after fields written
}

void apcKey25::handleMidi(u8 status, u8 data1, u8 data2)
{
    u8 msgType = status & 0xF0;
    u8 channel = status & 0x0F;

    if (msgType == APC_CH_NOTE_ON && data2 > 0)
    {
        // SHIFT lives on the BUTTON channel (0). Without the channel guard the
        // keybed (channel 1) note 98 == APC_BTN_SHIFT (0x62) -- which is the D
        // three octaves up -- was swallowed as SHIFT and never played. The keybed
        // (ch1) must be handled ONLY by the channel==1 block below; no button
        // function may intercept it.
        if (channel == 0 && data1 == APC_BTN_SHIFT) { m_shift = true; return; }
        if (channel == 0 && data1 == 64) {
            m_liveEngaged = !m_liveEngaged;
            if (!m_liveEngaged) m_livePitchSemitones = 0.0f;
            _applyLivePitch();
            return;
        }
        if (channel == 1) {
            // Sampler takes the keys when it has content. In drum-record mode
            // (button 66 held) a key press records into THAT key's drum slot.
            // Otherwise, if a chromatic sample is loaded OR this key has a drum
            // slot, the key triggers sampler playback (live-pitch keyboard
            // transpose is suppressed; live pitch stays reachable via mod-wheel /
            // CC52). With no sampler content the key falls through to live pitch.
            if (pSampler) {
                int keyIdx = sampler::keyIndex((int)data1);
                if (m_drumRecordMode) {
                    if (keyIdx >= 0)
                        pSampler->pushEvent(sampler::EV_REC_START, keyIdx, 0);
                    return;
                }
                if (pSampler->chromaticLoaded() || pSampler->drumLoaded(keyIdx)) {
                    pSampler->pushEvent(sampler::EV_NOTE_ON, (int)data1, (int)data2);
                    return;
                }
            }
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
        // Microrepeat latch notes 82-86 (1, 1/2, 1/4, 1/8, 1/16 beat). Checked
        // BEFORE the pad/button dispatch so note 84 overrides APC_BTN_FORMAT.
        // Held = latched; last-pressed wins. Released in the note-off block.
        // BUTTON channel (0) only -- a keybed (ch1) note 82-86 must play, not latch.
        if (channel == 0 && data1 >= 82 && data1 <= 86) {
            static const u8 div[5] = {1, 2, 4, 8, 16};   // 82..86 -> 1/div beat
            m_microRepeatDiv = div[data1 - 82];
            return;
        }
        // Sampler control buttons (free track buttons 65/66, channel 0). Checked
        // BEFORE the pad/button dispatch. 65 HELD = record the shared chromatic
        // sample; 66 HELD = drum-record-arm (keys record into per-key slots).
        if (channel == 0 && data1 == 65) {
            if (pSampler) pSampler->pushEvent(sampler::EV_REC_START, -1, 0);  // -1 = chromatic
            return;
        }
        if (channel == 0 && data1 == 66) {
            m_drumRecordMode = true;
            g_samplerDrumMode = 1;
            return;
        }
        // Pads + transport buttons are BUTTON channel (0) only -- never the keybed.
        if (channel == 0 && data1 < APC_ROWS * APC_COLS)
        {
            _onPadPress(data1 / APC_COLS, data1 % APC_COLS);
            return;
        }
        if (channel == 0) { _onButton(data1); return; }
        return;
    }

    if (msgType == APC_CH_NOTE_OFF || (msgType == APC_CH_NOTE_ON && data2 == 0))
    {
        // SHIFT + microrepeat latches are BUTTON-channel (0) functions. Guard them
        // by channel so a keybed (ch1) note-off -- e.g. note 98 (D, +3 oct) ==
        // APC_BTN_SHIFT, or notes 82-86 at +octaves -- is NOT intercepted and
        // reaches the channel==1 keybed release below (otherwise voices stick).
        if (channel == 0 && data1 == APC_BTN_SHIFT) { m_shift = false; return; }
        // Release a microrepeat latch: clear only if the released note owns the
        // currently-active division (so releasing a stale earlier note doesn't
        // cancel a newer held one).
        if (channel == 0 && data1 >= 82 && data1 <= 86) {
            static const u8 div[5] = {1, 2, 4, 8, 16};
            if (m_microRepeatDiv == div[data1 - 82]) m_microRepeatDiv = 0;
            return;
        }
        // Sampler button releases.
        if (channel == 0 && data1 == 65) {
            if (pSampler) pSampler->pushEvent(sampler::EV_REC_STOP, 0, 0);   // stop + auto-trim
            return;
        }
        if (channel == 0 && data1 == 66) {
            m_drumRecordMode = false;
            g_samplerDrumMode = 0;
            // Stop any in-progress drum capture (idempotent if none) so releasing
            // 66 before the key never leaves a record armed.
            if (pSampler) pSampler->pushEvent(sampler::EV_REC_STOP, 0, 0);
            return;
        }
        if (channel == 0 && data1 == 64) return;
        if (channel == 1) {
            // Mirror the note-on routing: stop a drum-record on key release, or
            // send the sampler a NOTE_OFF. The NOTE_OFF is forwarded
            // UNCONDITIONALLY (not gated on chromaticLoaded/drumLoaded) -- gating
            // it could SUPPRESS the release if content changed between press and
            // release, stranding a sustaining voice (the "auto-sustain" bug). An
            // unmatched NOTE_OFF is harmless in the sampler.
            if (pSampler) {
                if (m_drumRecordMode) {
                    int keyIdx = sampler::keyIndex((int)data1);
                    if (keyIdx >= 0)
                        pSampler->pushEvent(sampler::EV_REC_STOP, 0, 0);
                    return;
                }
                pSampler->pushEvent(sampler::EV_NOTE_OFF, (int)data1, 0);
            }
            m_transposeLocked = false;
            return;
        }
        // Pad releases are BUTTON channel (0) only.
        if (channel == 0 && data1 < APC_ROWS * APC_COLS)
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
        p.microRepeatDiv     = m_microRepeatDiv;  // latched beat-repeat (notes 82-86)
        p.monitorMode        = m_shift;   // SHIFT held = temporary input-monitor
                                          // (gate loop output, hear live input)
        // Ableton Link shared beat phase (full Link sync). linkGhostPhase is a
        // Core-2-local read (abletonLink runs on this same control plane), then
        // published in the snapshot for Core 1's loopMachine to align masterPhase.
        {
            s64 ph = 0, q = 4000000;
            p.linkPhaseValid          = linkGhostPhase(&ph, &q);
            p.linkBeatPhaseMicroBeats = ph;
            p.linkQuantumMicroBeats   = q;
        }
        paramSnapshotPublish(p);
    }

    if (m_liveLedDirty)
    {
        // Drop-retry: clear dirty ONLY if the frame was actually queued
        // (usbMidiSendNoteOn returns false when MIDI OUT is full). Previously
        // this used fire-and-forget usbMidiSend() and cleared the flag
        // unconditionally, so a dropped frame left the live-engage LED stuck on
        // its old state forever. Now a drop leaves m_liveLedDirty set and the
        // next tick retries — same consistency guarantee the grid LEDs get from
        // sendLedCoalesced's retry-on-drop cache.
        // Live-engage LED note + on/off velocity come from the active profile
        // (midiMap.h) so a remapped controller can relocate/recolour it.
        const MidiOutputMap* lo =
            midiMapResolveOutput(g_activeProfile,
                                 m_liveEngaged ? MFS_LIVE_ENGAGE_ON : MFS_LIVE_ENGAGE_OFF);
        u8 liveVel = lo ? lo->velocity : (m_liveEngaged ? 127 : 0);
        if (usbMidiSendNoteOn(APC25_LIVE_LED_NOTE, liveVel))
            m_liveLedDirty = false;
    }

    m_nowMs = CTimer::Get()->GetClockTicks() / 1000;

    if (m_bootMs == 0) m_bootMs = m_nowMs;

    // Drain ALL queued commands this tick (in press order) so nothing waits an
    // extra tick or gets dropped — keeps every looper's response prompt + equal.
    while (m_cmdTail != m_cmdHead)
    {
        ApcCmd::Type type = m_cmdRing[m_cmdTail].type;
        int arg = m_cmdRing[m_cmdTail].arg;
        // Publish the press instant for this command so record start/stop can
        // backdate to it (read in the same Core-2 call, single-threaded).
        g_pendingPressTicks = m_cmdRing[m_cmdTail].press_ticks;
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
            // CLEAR_ALL empties every looper — every arrangement is now empty,
            // so forget them all and let their LEDs go dark.
            if (arg == LOOP_COMMAND_CLEAR_ALL)
                _forgetAllPresets();
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
                // Hold = stop+clear, ALWAYS ends with the LED off. Route to
                // ERASE_TRACK (not CLEAR_LAYER): ERASE_TRACK unconditionally
                // clears content + cancels any pending deferred-record and
                // never re-arms, whereas CLEAR_LAYER on an empty/just-erased
                // track re-arms a pending RECORD when a master grid exists,
                // leaving the pad yellow (the "LED didn't disappear" bug).
                pTheLooper->command(LOOP_COMMAND_ERASE_TRACK_BASE + n);
                // The looper is now empty — drop it from every arrangement
                // and delete any arrangement it was the last member of.
                _forgetLooperFromPresets(n);
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