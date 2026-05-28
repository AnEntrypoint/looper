// continuousBuffer.h — the always-on 3-minute rolling stereo record buffer.
//
// THIS IS THE SINGLE STAGING AREA FOR ALL LOOPER MEMORY. The audio update
// writes every input block into it unconditionally, advancing a monotonic
// absolute block counter. Loopers do NOT record live into their own buffers;
// they record a clip by remembering a [startBlock, stopBlock) range of
// ABSOLUTE block indices into this buffer and copying that range out at commit.
//
// Why this makes latency-backdating exact and trivial: the audio the musician
// heard at the instant they pressed a button is ALWAYS still resident in this
// buffer (until it wraps ~3 min later), so a backdated start is just an earlier
// absolute block index — no pre-roll gymnastics, no per-clip rewind.
//
// Single writer (Core 1 audio update), readers copy-on-commit before the range
// can wrap. Monotonic g_cbWriteBlock never resets; physical slot = block %
// CB_RING_BLOCKS. A clip range older than CB_RING_BLOCKS has been overwritten —
// callers clamp to that horizon (see cbBlocksAvailable).

#ifndef CONTINUOUS_BUFFER_H
#define CONTINUOUS_BUFFER_H

#include "AudioTypes.h"

// ~3 minutes of stereo @ the internal rate. INTEGRAL_BLOCKS_PER_SECOND is ~345
// (44100/128-ish per AudioTypes/Looper.h). 180s * 345 ≈ 62100 blocks. Each
// block is AUDIO_BLOCK_SAMPLES stereo s16 = 64*2*2 = 256 bytes => ~15.9 MB.
// (Defined as a round power-of-two-friendly count for cheap modulo via masking
//  is NOT required — modulo by a constant is fine on the audio core and keeps
//  the size exactly 3 minutes, predictable over clever.)
#define CB_RING_SECONDS   180
#define CB_RING_BLOCKS    (CB_RING_SECONDS * INTEGRAL_BLOCKS_PER_SECOND)

// Interleaved stereo s16, AUDIO_BLOCK_SAMPLES per channel per block.
extern s16      g_cbBuffer[CB_RING_BLOCKS * AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS];
// Monotonic absolute block index of the NEXT block to be written. Advanced by
// one each audio update after the block is stored. Never resets.
extern volatile u32 g_cbWriteBlock;

// Write one stereo block (interleaved L,R,L,R... AUDIO_BLOCK_SAMPLES frames)
// into the rolling buffer and advance the write head. Called every audio block
// from loopMachine::update, always-on, before any per-track processing.
void cbWriteBlock(const s32 *interleavedStereoBlock);

// Pointer to the stored stereo block at absolute index `block` (interleaved
// s16). Valid only while `block` is within the live horizon (see below).
inline s16 *cbBlockPtr(u32 block)
{
    return &g_cbBuffer[(block % CB_RING_BLOCKS) * AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS];
}

// How many blocks of history are safely readable behind the current write head
// (== the full ring once warmed up; less right after boot). A backdate request
// is clamped to this so a read never crosses the wrap into overwritten audio.
inline u32 cbBlocksAvailable(void)
{
    u32 wr = g_cbWriteBlock;
    return wr < CB_RING_BLOCKS ? wr : CB_RING_BLOCKS;
}

// Copy a [startBlock, stopBlock) absolute range out of the rolling buffer into
// `dst` (interleaved s16, must hold (stopBlock-startBlock)*AUDIO_BLOCK_SAMPLES*
// LOOPER_NUM_CHANNELS samples). Handles the physical wrap internally. Caller
// guarantees the range is within the live horizon (clamped via cbBlocksAvailable).
void cbCopyRange(u32 startBlock, u32 stopBlock, s16 *dst);

// ---- Latency backdating ---------------------------------------------------
// Fixed lag (samples) between the input the musician HEARD and what the audio
// chain is currently processing: the deliberate USB-IN ring write-vs-read gap
// (IN_TARGET_LAG=96, input_usb.cpp) plus the ADC/USB transport. Kept as one
// documented constant; predictable over clever.
#define CB_FIXED_LAG_SAMPLES 96

// Telemetry: last applied backdate (samples) + last press->process latency (us)
// + whether the last backdate was clamped to the history horizon. For :4445.
extern volatile u32 g_cbLastBackdateSamples;
extern volatile u32 g_cbLastPressLatencyUs;
extern volatile u32 g_cbLastBackdateClamped;

// Press timestamp (CTimer us) of the command currently being dispatched. The
// APC drain (apcKey25::update, Core 2) sets this immediately before calling
// pTheLooper->command(); record start/stop read it the same Core-2 call (single
// thread, no race) to backdate to the press instant. 0 between commands.
extern volatile unsigned g_pendingPressTicks;

// Given a press timestamp (CTimer microseconds, 0 = none) captured at the MIDI
// ISR, return the ABSOLUTE rolling-buffer block index that corresponds to the
// press instant: current write head minus the backdate (press->now elapsed +
// fixed lag), converted to blocks, wrap-safe and clamped to the live horizon so
// the returned block is always readable. This is THE backdate primitive.
u32 cbBackdatedBlock(unsigned press_ticks);

#endif
