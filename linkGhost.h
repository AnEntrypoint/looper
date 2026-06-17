// linkGhost.h — Ableton Link ghost-clock transform + measurement aggregation.
// LAYER (measurement core) of full Link. Pure (stdint only, no Circle), so it
// compiles into both the firmware and the host test (scripts/test-link-ghost.cpp).
//
// A Link session has a shared "ghost" beat-clock. Each peer measures the offset
// between its local host clock (microseconds) and the session ghost clock via the
// unicast PING/PONG exchange (see linkWire.h lwMeasurementSamples). The reference
// MeasurementService computes GhostXForm{slope=1, offset=median(samples)} after
// collecting up to kNumberDataPoints=100 samples over kNumberMeasurements=5 pings
// at 50ms intervals. Slope is fixed at 1 (clocks assumed same rate; only the
// constant offset is measured), so:
//     ghost = host + offset      host = ghost - offset
//
// This module owns: the GhostXForm map, and the per-peer measurement accumulator
// (sample collection + median + done/timeout predicates). The transport (sending
// pings, receiving pongs over unicast WiFi) and session-timeline ownership are
// separate layers; this is the math they feed.

#ifndef _linkGhost_h
#define _linkGhost_h

#include <stdint.h>

// Reference constants (Measurement.hpp).
#define LG_NUM_DATAPOINTS    100   // target samples before a measurement is "done"
#define LG_NUM_MEASUREMENTS  5     // max pings per measurement burst
#define LG_PING_INTERVAL_US  50000 // 50 ms between pings
#define LG_MAX_SAMPLES       256   // accumulator capacity (>= 2 * data points headroom)

// ---- ghost-clock transform (slope fixed at 1; constant offset) ----
typedef struct { int64_t offsetMicros; } LinkGhostXForm;

static inline int64_t lgHostToGhost(LinkGhostXForm x, int64_t hostMicros)  { return hostMicros + x.offsetMicros; }
static inline int64_t lgGhostToHost(LinkGhostXForm x, int64_t ghostMicros) { return ghostMicros - x.offsetMicros; }

// ---- measurement accumulator ----
typedef struct {
    int64_t  samples[LG_MAX_SAMPLES];
    int      n;
    int      pings;          // pings issued in this burst
    int64_t  lastPingMicros; // host time of the last ping sent (scheduler cadence)
    // initiator-side history for the sample formula:
    int64_t  prevHostSend;   // send time of the previous ping (0 = none yet)
    int64_t  prevGhost;      // ghost time from the previous pong (0 = none yet)
} LinkMeasurement;

static inline void lgMeasReset(LinkMeasurement *m)
{
    m->n = 0; m->pings = 0; m->lastPingMicros = 0; m->prevHostSend = 0; m->prevGhost = 0;
}

// Add raw offset samples (already derived via lwMeasurementSamples). Clamps to
// capacity so a chatty peer can't overflow the buffer.
static inline void lgMeasAddSamples(LinkMeasurement *m, const int64_t *s, int k)
{
    for (int i = 0; i < k && m->n < LG_MAX_SAMPLES; i++) m->samples[m->n++] = s[i];
}

// Median of the collected samples (insertion-sort a local copy; n is small and
// bounded). Returns 0 when empty.
static inline int64_t lgMeasMedian(const LinkMeasurement *m)
{
    if (m->n <= 0) return 0;
    int64_t a[LG_MAX_SAMPLES];
    int n = m->n;
    for (int i = 0; i < n; i++) a[i] = m->samples[i];
    for (int i = 1; i < n; i++) {
        int64_t key = a[i]; int j = i - 1;
        while (j >= 0 && a[j] > key) { a[j+1] = a[j]; j--; }
        a[j+1] = key;
    }
    if (n & 1) return a[n/2];
    return (a[n/2 - 1] + a[n/2]) / 2;
}

static inline bool lgMeasHasEnough(const LinkMeasurement *m) { return m->n >= LG_NUM_DATAPOINTS; }
static inline bool lgMeasExhausted(const LinkMeasurement *m) { return m->pings >= LG_NUM_MEASUREMENTS; }

// The measurement result the session adopts: GhostXForm{1, median}. Caller checks
// lgMeasHasEnough() first; an exhausted-but-insufficient burst is a failed measure.
static inline LinkGhostXForm lgMeasResult(const LinkMeasurement *m)
{
    LinkGhostXForm x; x.offsetMicros = lgMeasMedian(m); return x;
}

// ---- session timeline (the 'tmln' payload, ghost-clock domain) ----
// tempo as microseconds-per-beat (i64), beatOrigin as MICROBEATS (beats*1e6,
// i64, matching Link's Beats), timeOrigin as ghost microseconds (i64). The beat
// at a ghost time t:  beats = beatOrigin_beats + (t - timeOrigin)/microsPerBeat.
// We keep everything in microbeats to stay integer/exact:
//   microBeats(t) = beatOriginMicroBeats + (t - timeOrigin) * 1e6 / microsPerBeat
typedef struct {
    int64_t tempoMicrosPerBeat;    // 60e6 / bpm
    int64_t beatOriginMicroBeats;  // beats * 1e6 at timeOrigin
    int64_t timeOriginMicros;      // ghost-clock microseconds
} LinkTimeline;

static inline int64_t lgBpmToMicrosPerBeat(double bpm) { return (int64_t)(60000000.0 / bpm + 0.5); }
static inline double  lgMicrosPerBeatToBpm(int64_t mpb) { return mpb > 0 ? 60000000.0 / (double)mpb : 0.0; }

// Microbeats at ghost time t. NOTE (link-clock-units): (t-timeOrigin) is bounded
// by the session age in micros; * 1e6 stays within int64 for any realistic
// session (hours). Link re-anchors beatOrigin/timeOrigin so the delta stays small.
static inline int64_t lgTimelineMicroBeatsAt(LinkTimeline tl, int64_t ghostMicros)
{
    if (tl.tempoMicrosPerBeat <= 0) return tl.beatOriginMicroBeats;
    return tl.beatOriginMicroBeats
         + ((ghostMicros - tl.timeOriginMicros) * 1000000LL) / tl.tempoMicrosPerBeat;
}

// Phase within a quantum (quantum given in microbeats, e.g. 4 beats = 4*1e6).
// Always in [0, quantumMicroBeats).
static inline int64_t lgBeatPhaseMicro(int64_t microBeats, int64_t quantumMicroBeats)
{
    if (quantumMicroBeats <= 0) return 0;
    int64_t p = microBeats % quantumMicroBeats;
    if (p < 0) p += quantumMicroBeats;
    return p;
}

// Convenience: beat phase at a HOST time, given our ghost transform + the session
// timeline. This is what the looper aligns masterPhase to.
static inline int64_t lgBeatPhaseAtHost(LinkGhostXForm x, LinkTimeline tl,
                                        int64_t hostMicros, int64_t quantumMicroBeats)
{
    int64_t ghost = lgHostToGhost(x, hostMicros);
    int64_t mb = lgTimelineMicroBeatsAt(tl, ghost);
    return lgBeatPhaseMicro(mb, quantumMicroBeats);
}

#endif
