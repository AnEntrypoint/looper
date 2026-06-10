#ifndef MICRO_REPEAT_H
#define MICRO_REPEAT_H

#include <stdint.h>
#include <string.h>

// Synced latch-based microrepeat (beat-repeat / stutter).
//
// While a division is latched, this stage repeats a beat-fraction slice of the
// FULL pre-output mix (live input + all loops) in sync with the master beat
// grid. Divisions: 1, 1/2, 1/4, 1/8, 1/16 beat (notes 82..86).
//
// LOAD-BEARING INVARIANTS:
//  - The repeat is EPHEMERAL and NON-DESTRUCTIVE to playback position. This
//    stage NEVER touches the loop clips' play heads or masterPhase; the loops
//    keep advancing underneath while latched, so on release the signal resumes
//    exactly the position it would have had if the repeat never happened. (The
//    caller guarantees this by keeping the clip advance independent of the
//    stage — see loopMachine::update.)
//  - It taps the FULL mix so ALL loops stutter, regardless of SHIFT. The caller
//    hands process() the combined (input + loop sum) signal and writes the
//    result to both the hardware out AND the record buffer, so under SHIFT the
//    stutter is recorded into a loop.
//  - Beat-synced: the slice length and its phase are derived from masterPhase /
//    masterLoopBlocks every block, so the repeat restarts on the grid and stays
//    locked to the loops.
//  - Click-free: a per-sample wet ramp crossfades live<->repeated on engage and
//    release; the slice is a whole number of samples captured contiguously, so
//    its wrap is phase-continuous (no seam discontinuity in a steady signal).
//
// Bare-metal: fixed-size buffers, no allocation in the audio path. The capture
// ring holds the longest supported slice (1 beat at the slowest tempo).
class microRepeat {
public:
    // Per-channel capture ring. 1 beat at the slowest supported tempo
    // (~80 BPM -> phrase ~8280 blocks -> beat ~518 blocks -> ~33k samples).
    // 48000 (~1s @ 48k) is a safe ceiling with headroom.
    static const int MR_MAX_SLICE = 48000;
    static const int MR_CHANNELS  = 2;

    microRepeat() { reset(); }

    void reset() {
        memset(m_ring, 0, sizeof(m_ring));
        m_writePos   = 0;
        m_filled     = 0;
        m_activeDiv  = 0;
        m_sliceLen   = 0;
        m_readPos    = 0;
        m_wet        = 0.0f;
    }

    // One audio block. inout points at the FULL mix for this block, layout
    // matching loopMachine's m_input_buffer: [L0..L_{N-1}, R0..R_{N-1}].
    // div: 0 = off, else 1/2/4/8/16 (the beat divisor). masterLoopBlocks: the
    // phrase length in blocks (16 beats); 0 = no grid yet. Returns nothing; on
    // return inout holds the stage output (repeated slice while latched, live
    // signal otherwise), crossfaded click-free.
    void process(int *inout, uint32_t /*masterPhase*/, uint32_t masterLoopBlocks,
                 uint8_t div, int blockSamples)
    {
        const int N = blockSamples;

        // Derive the slice length (samples per channel) from the beat grid.
        // beat_blocks = masterLoopBlocks / 16; slice_blocks = beat_blocks / div.
        // No grid (masterLoopBlocks==0) or div==0 -> repeat disabled.
        int sliceLen = 0;
        if (div != 0 && masterLoopBlocks >= 16) {
            uint32_t beatBlocks  = masterLoopBlocks / 16;          // 1 beat
            uint32_t sliceBlocks = beatBlocks / (uint32_t)div;     // 1/div beat
            if (sliceBlocks < 1) sliceBlocks = 1;                  // floor: >=1 block
            sliceLen = (int)(sliceBlocks * (uint32_t)N);
            if (sliceLen > MR_MAX_SLICE) sliceLen = MR_MAX_SLICE;
        }

        bool wantActive = (sliceLen > 0);

        // ENGAGE / RETARGET: on a fresh latch (or a div change that changes the
        // slice length) capture the slice starting now and anchor the read head
        // to the slice start, so the repeat begins grid-aligned and seamless.
        if (wantActive && (m_activeDiv != div || m_sliceLen != sliceLen)) {
            m_activeDiv = div;
            m_sliceLen  = sliceLen;
            // Anchor the read head at the start of the slice we are about to
            // record (the live audio from this instant becomes the loop).
            m_readPos   = 0;
            m_capturePos = 0;
            m_capturing  = true;
        }
        if (!wantActive) {
            m_activeDiv = 0;
            m_sliceLen  = 0;
        }

        // Per-block wet ramp endpoints (click-free engage/release).
        float wetStart  = m_wet;
        float wetTarget = wantActive ? 1.0f : 0.0f;
        float wetEnd    = wetStart;
        const float STEP = 1.0f / 16.0f;
        if (wetEnd < wetTarget) { wetEnd += STEP; if (wetEnd > wetTarget) wetEnd = wetTarget; }
        else if (wetEnd > wetTarget) { wetEnd -= STEP; if (wetEnd < wetTarget) wetEnd = wetTarget; }
        m_wet = wetEnd;
        float wetSampStep = (wetEnd - wetStart) / (float)N;
        float wet = wetStart;

        for (int i = 0; i < N; i++) {
            int liveL = inout[i];
            int liveR = inout[N + i];

            // While a slice is being captured (the first slice-length samples
            // after engage), record the live audio into the ring so the loop
            // content == the audio at the moment of the press.
            int repL = liveL, repR = liveR;
            if (m_sliceLen > 0) {
                if (m_capturing) {
                    m_ring[0][m_capturePos] = liveL;
                    m_ring[1][m_capturePos] = liveR;
                    m_capturePos++;
                    if (m_capturePos >= m_sliceLen) { m_capturePos = 0; m_capturing = false; }
                    // During capture, the repeated signal IS the live signal
                    // (first pass through the slice) -> seamless into the loop.
                    repL = liveL; repR = liveR;
                } else {
                    repL = m_ring[0][m_readPos];
                    repR = m_ring[1][m_readPos];
                }
                // Advance the read head, wrapping at the slice boundary (the
                // grid-aligned loop point).
                m_readPos++;
                if (m_readPos >= m_sliceLen) m_readPos = 0;
            }

            // Crossfade live <-> repeated by the wet gain (click-free).
            inout[i]     = (int)((float)liveL * (1.0f - wet) + (float)repL * wet);
            inout[N + i] = (int)((float)liveR * (1.0f - wet) + (float)repR * wet);
            wet += wetSampStep;
        }
    }

    uint8_t activeDiv() const { return m_activeDiv; }
    bool    isActive()  const { return m_wet > 0.0001f || m_activeDiv != 0; }
    // Block-end wet/crossfade gain (0 = fully live, 1 = fully repeated). The
    // caller gates the dry loop add in the final mix by (1 - wet) so the loop
    // sum is counted exactly once (it is already inside m_input_buffer, hence
    // the stage output, in proportion to wet).
    float   wet() const { return m_wet; }
    int     sliceLenForTest() const { return m_sliceLen; }

private:
    int m_ring[MR_CHANNELS][MR_MAX_SLICE];
    int     m_writePos;     // (reserved for future continuous-capture modes)
    int     m_filled;       // (reserved)
    int     m_capturePos;   // write index while capturing the slice
    bool    m_capturing;    // true while the first slice is being recorded
    uint8_t m_activeDiv;    // currently latched division (0 = off)
    int     m_sliceLen;     // slice length in samples per channel
    int     m_readPos;      // replay read index within the slice
    float   m_wet;          // engage/release crossfade gain (0..1)
};

#endif // MICRO_REPEAT_H
