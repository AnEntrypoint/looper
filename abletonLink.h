#ifndef _abletonLink_h
#define _abletonLink_h

#include <wlan/bcm4343.h>
#include <circle/types.h>

void linkInit(CBcm4343Device *pWLAN);
void linkProcess(void);
double linkGetBPM(void);
void linkSetBPM(double bpm);
bool linkIsSynced(void);

// ---- Timeline (train-on-first-loop) --------------------------------------
// On rec-tap ON with no clips: mark "have started" and anchor the Link timeline
// origin at the (backdated) press instant. originTicks = CTimer microseconds of
// the press; 0 = now. The next sendAlive broadcasts this origin so peers (Ableton
// Live) treat the first loop as beat 0 / song start.
void linkStart(unsigned originTicks);

// On rec-tap OFF: "have ended". Derive tempo + quant from the recorded loop
// length (clip_seconds): choose the musical beat-count whose resulting BPM is
// nearest 120 within [80,160], set s_bpm from it, mark the timeline finalized so
// sendAlive broadcasts the new origin+mpb. Returns chosen beats (the quant unit).
double linkEnd(double clip_seconds);

// Pure helper (also unit-tested host-side): nearest-120 beat-count + bpm for a
// given loop length. beats in {0.25,0.5,1,2,4,8,16}.
void linkDeriveQuant(double clip_seconds, double *out_beats, double *out_bpm);

bool   linkHasStarted(void);   // timeline anchored (rec started, not yet ended)
bool   linkHasEnded(void);     // timeline finalized (loop length known)
double linkQuantBeats(void);   // chosen quant subdivision in beats (0 if none)
bool   linkIsPlaying(void);    // real Link transport Start/Stop state (see abletonLink.cpp s_isPlaying)

// Call when a full erase drops the bank back to empty so the NEXT first loop
// re-triggers its own Start/Stop broadcast instead of being skipped because a
// prior (now-erased) phrase already set the transport playing.
void linkResetTransport(void);

// Shared ghost beat phase from the adopted session timeline. Returns false until
// a valid session phase exists. phaseMicroBeats in [0, quantumMicroBeats). Read
// on Core 2 (apcKey25::update) and published into the paramSnapshot for Core 1.
bool linkGhostPhase(s64 *phaseMicroBeats, s64 *quantumMicroBeats);

// Telemetry snapshot for the :4445 LINK verb.
void linkTelemetry(unsigned *peers, s64 *offsetUs, unsigned *pingsTx,
                   unsigned *pongsRx, int *selfOwns);

// Best-effort BYEBYE multicast on shutdown so peers drop us promptly.
void linkShutdown(void);

#endif
