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

u8 apcKey25::_trackLedColor(int track)
{
    if (track >= LOOPER_NUM_TRACKS) return APC_VEL_LED_OFF;
    publicTrack *pTrack = pTheLooper->getPublicTrack(track);
    u16 ts = pTrack->getTrackState();

    if (ts & TRACK_STATE_RECORDING)
        return pTrack->getNumRecordedClips() > 0 ? APC_VEL_LED_YELLOW : APC_VEL_LED_RED;
    if (ts & (TRACK_STATE_PENDING_RECORD | TRACK_STATE_PENDING_PLAY | TRACK_STATE_PENDING_STOP))
        return APC_VEL_LED_YELLOW;
    if (ts & TRACK_STATE_PLAYING)
        return APC_VEL_LED_GREEN;
    return APC_VEL_LED_OFF;
}

u8 apcKey25::_muteLedColor(int track)
{
    if (track >= LOOPER_NUM_TRACKS) return APC_VEL_LED_OFF;
    publicTrack *pTrack = pTheLooper->getPublicTrack(track);
    int layers = pTrack->getNumRecordedClips();
    if (layers == 0) return APC_VEL_LED_OFF;
    bool stopped = (pTrack->getTrackState() & TRACK_STATE_STOPPED) != 0;
    u8 color = APC_VEL_LED_GREEN;
    if (layers >= 3) color = APC_VEL_LED_RED;
    else if (layers >= 2) color = APC_VEL_LED_YELLOW;
    if (stopped) color++;
    return color;
}

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
    CLogger::Get()->Write(log_name, LogNotice,
        "livePitch engaged=%d semis=%.2f",
        m_liveEngaged ? 1 : 0,
        m_livePitchSemitones);
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
    for (int row = 0; row < LOOPER_NUM_TRACKS; row++)
    {
        u8 col0 = _trackLedColor(row);
        u8 col1 = _muteLedColor(row);
        sendLedCoalesced(_padNote(row, 0), col0);
        sendLedCoalesced(_padNote(row, 1), col1);
    }

    // Per-clip VU grid: cols 2..5 (4 layers) × rows 0..4 (5 tracks).
    // Each pad shows its OWN clip's level as blank/green/yellow/red.
    // Drains m_clipPeakLevel on read so it tracks peaks-since-last-update.
    for (int track = 0; track < LOOPER_NUM_TRACKS; track++)
    {
        publicTrack *pTrack = pTheLooper->getPublicTrack(track);
        // Track-level peak still drained here to keep its accumulator bounded
        // even though we're not painting a track-VU bar anymore.
        pTrack->m_peakLevel = 0;
        for (int layer = 0; layer < LOOPER_NUM_LAYERS; layer++)
        {
            int col = 2 + layer;
            if (col >= APC_COLS) break;
            publicClip *pClip = pTrack->getPublicClip(layer);
            u8 color = APC_VEL_LED_OFF;
            if (pClip) {
                u32 cpeak = pClip->m_clipPeakLevel;
                pClip->m_clipPeakLevel = 0;
                if      (cpeak > 8000) color = APC_VEL_LED_RED;
                else if (cpeak > 1500) color = APC_VEL_LED_YELLOW;
                else if (cpeak > 200)  color = APC_VEL_LED_GREEN;
            }
            sendLedCoalesced(_padNote(track, col), color);
        }
        // Cols 6,7 explicitly cleared so stale LED state never lingers there.
        for (int col = 2 + LOOPER_NUM_LAYERS; col < APC_COLS; col++) {
            sendLedCoalesced(_padNote(track, col), APC_VEL_LED_OFF);
        }
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
