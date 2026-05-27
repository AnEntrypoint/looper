#define log_name "apc25"

#include "apcKey25.h"
#include "input_usb.h"
#include "Looper.h"
#include "patches/RubberBandWrapper.h"
#include "patches/paramSnapshot.h"
#include "abletonLink.h"
#include "usbMidi.h"
#include <circle/logger.h>
#include <circle/util.h>

extern apcKey25 *pTheAPC;
extern publicLoopMachine *pTheLooper;

void apcKey25::_updateComputedRatio()
{
    float semitones = (float)m_transposePitch + m_pitchWheelOffset;
    m_computedRatio = pow(2.0f, semitones / 12.0f);
}

void apcKey25::_updateDrift()
{
    if (m_pitchWheelOffset != m_driftTarget)
    {
        unsigned long elapsedMs = m_nowMs - m_lastDriftMs;
        float driftPerMs = 0.01f;
        float drift = driftPerMs * elapsedMs;
        if (m_pitchWheelOffset > m_driftTarget + 0.5f)
            m_pitchWheelOffset -= drift;
        else if (m_pitchWheelOffset < m_driftTarget - 0.5f)
            m_pitchWheelOffset += drift;
        else
            m_pitchWheelOffset = m_driftTarget;
        m_lastDriftMs = m_nowMs;
        _updateComputedRatio();
    }
}

// _trackLedColor / _muteLedColor removed — the simplified UI (20 flat
// loopers + 10 preset slots) no longer has a per-track state column or
// per-track mute column. All looper state encoding now lives in the
// looper-pad LED logic in _updateGridLeds.

void apcKey25::_applyLivePitch()
{
    // Publish to snapshot; Core 1 DSP applies pitch via setPitchScale to keep
    // signalsmith state single-writer. Cross-core safe.
    LiveParams p = paramSnapshotLoad();
    p.liveEngaged        = m_liveEngaged;
    p.livePitchSemitones = m_livePitchSemitones;
    p.linkSynced         = linkIsSynced();
    p.linkBPM            = (float)linkGetBPM();
    paramSnapshotPublish(p);
    m_liveLedDirty = true;
    // No syslog per CC — knob sweeps generate ~100 CCs/sec and each
    // CLogger::Write blocks long enough to cause USB-IN ring underruns =
    // audible clicks. State is observable via the periodic stat block.
}

apcKey25::DebugState apcKey25::getDebugState() const
{
    return {
        m_transposeLocked,
        m_transposePitch,
        (float)m_pitchWheelOffset,
        m_driftTarget,
        m_computedRatio,
        m_liveEngaged,
        m_livePitchSemitones
    };
}

apcKey25::EffectsState apcKey25::getEffectsState() const
{
    return {
        m_filterHP,
        m_filterLP,
        m_filterRes,
        m_reverbAmount,
        m_delayAmount,
        m_time,
        m_formant
    };
}

static u8 s_lastLedState[128];
static bool s_ledCacheValid = false;

void apcKey25::invalidateLedCache()
{
    s_ledCacheValid = false;
}

static void sendLedCoalesced(u8 note, u8 vel)
{
    if (!s_ledCacheValid) {
        for (int i = 0; i < 128; i++) s_lastLedState[i] = 0xFF;
        s_ledCacheValid = true;
    }
    if (s_lastLedState[note] == vel) return;
    // Only commit to cache when the send is actually queued. If MIDI OUT is
    // full and the frame drops, leave the cache showing the old value so the
    // next tick retries — otherwise dropped LED updates freeze pads in stale
    // colors (the "stuck VU LEDs" symptom).
    if (usbMidiSendNoteOn(note, vel)) {
        s_lastLedState[note] = vel;
    }
}

void apcKey25::_updateGridLeds()
{
    // Cols 0-1: 10 preset slots, idx = row*2 + col.
    //   blank: never captured
    //   green: captured + currently the "active" preset (last restored)
    //   yellow: captured but not the latest restore
    //   (we don't track "active" yet — for now: blank if unused, green if used)
    for (int row = 0; row < APC_ROWS; row++)
    {
        for (int col = 0; col < 2; col++)
        {
            int p = _presetFromPad(row, col);
            u8 color = APC_VEL_LED_OFF;
            if (p >= 0 && m_presetUsed[p])
                color = APC_VEL_LED_YELLOW;
            sendLedCoalesced(_padNote(row, col), color);
        }
    }

    // Cols 2-5: 20 loopers. Color encodes BOTH per-clip VU and state.
    // Empty looper: blank. Recording: red blink. Pending: yellow. Playing
    // (idle): green dim. Playing + audio: green→yellow→red by clip peak.
    for (int n = 0; n < LOOPER_NUM_TRACKS; n++)
    {
        publicTrack *pTrack = pTheLooper->getPublicTrack(n);
        if (!pTrack) continue;
        pTrack->m_peakLevel = 0;  // drain accumulator to bound it
        publicClip *pClip = pTrack->getPublicClip(0);
        u32 cpeak = 0;
        if (pClip) { cpeak = pClip->m_clipPeakLevel; pClip->m_clipPeakLevel = 0; }
        int ts = pTrack->getTrackState();

        u8 color = APC_VEL_LED_OFF;
        if (ts & TRACK_STATE_RECORDING) {
            color = APC_VEL_LED_RED_BLINK;   // red FLASH while recording (was solid)
        } else if (ts & (TRACK_STATE_PENDING_RECORD | TRACK_STATE_PENDING_PLAY | TRACK_STATE_PENDING_STOP)) {
            color = APC_VEL_LED_YELLOW;
        } else if (ts & TRACK_STATE_PLAYING) {
            if      (cpeak > 8000) color = APC_VEL_LED_RED;
            else if (cpeak > 1500) color = APC_VEL_LED_YELLOW;
            else                   color = APC_VEL_LED_GREEN;
        } else if (pTrack->getNumRecordedClips() > 0) {
            // Has content but paused/stopped — blink yellow so you can see
            // at a glance which loopers are loaded-but-silent vs truly empty.
            color = APC_VEL_LED_YELLOW_BLINK;
        }

        int row = n / 4;
        int col = 2 + (n % 4);
        sendLedCoalesced(_padNote(row, col), color);
    }

    // Cols 6,7 explicitly cleared every tick — no stale halve/double LEDs.
    for (int row = 0; row < APC_ROWS; row++) {
        sendLedCoalesced(_padNote(row, 6), APC_VEL_LED_OFF);
        sendLedCoalesced(_padNote(row, 7), APC_VEL_LED_OFF);
    }

    u32 peak = AudioInputUSB::s_peakLevel;
    AudioInputUSB::s_peakLevel = 0;
    int inVu = 0;
    if (peak > 100)   inVu = 1;
    if (peak > 500)   inVu = 2;
    if (peak > 2000)  inVu = 3;
    if (peak > 5000)  inVu = 4;
    if (peak > 10000) inVu = 5;

    u32 outPeak = pTheLooper->m_outputPeakLevel;
    pTheLooper->m_outputPeakLevel = 0;
    int outVu = 0;
    if (outPeak > 50)    outVu = 1;
    if (outPeak > 200)   outVu = 2;
    if (outPeak > 1000)  outVu = 3;
    if (outPeak > 4000)  outVu = 4;
    if (outPeak > 10000) outVu = 5;

    for (int i = 0; i < 5; i++)
    {
        u8 color = APC_VEL_LED_OFF;
        if (i < inVu)
            color = (i >= 4) ? APC_VEL_LED_RED : APC_VEL_LED_GREEN;
        else if (i < outVu)
            color = (i >= 4) ? APC_VEL_LED_RED : APC_VEL_LED_YELLOW;
        sendLedCoalesced(0x52 + i, color);
    }

    bool running = pTheLooper->getRunning();
    u16  pending = pTheLooper->getPendingCommand();
    sendLedCoalesced(APC_BTN_STOP_ALL, running ? (pending == LOOP_COMMAND_STOP ? APC_VEL_LED_YELLOW : APC_VEL_LED_GREEN) : APC_VEL_LED_OFF);
    sendLedCoalesced(APC_BTN_RECORD,   pTheLooper->getDubMode() ? APC_VEL_LED_RED : APC_VEL_LED_OFF);
    sendLedCoalesced(APC_BTN_PLAY,     pTheAPC->m_shift ? APC_VEL_LED_YELLOW : APC_VEL_LED_OFF);
}
