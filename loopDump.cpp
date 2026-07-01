#include "Looper.h"
#include "loopDump.h"
#include "usbWavRecorder.h"
#include <ff.h>
#include <circle/util.h>

// One-shot dump of every recorded looper track's clip buffer to its own WAV
// file on the USB flash drive. Reuses the "USB:" FatFs volume usbWavRecorder.cpp
// keeps mounted for the continuous ring recording -- this module never mounts
// or unmounts it, only writes files while g_uwavMounted is set, so the two
// never race for the mount itself. All FatFs calls happen from loopDumpTick(),
// driven from the same Core-2 control-plane tick as usbWavTick(), never from
// the audio ISR or a USB completion handler (LOOPER_LOG/blocking I/O there
// would stall audio -- see AGENTS.md).

#define LDUMP_HDR_BYTES        44
#define LDUMP_CHUNK_BLOCKS     64                                  // ~16KB write granularity
#define LDUMP_CHUNK_SAMPLES    (LDUMP_CHUNK_BLOCKS * AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS)
// Flat filenames at the volume root (no subdirectory: f_mkdir support depends
// on the FatFs build config, which lives outside this repo -- matching
// usbWavRecorder.cpp's own flat "USB:/looper-rec.wav" convention sidesteps
// the dependency entirely).
#define LDUMP_PATH_PREFIX      "USB:/looper-track"

volatile unsigned g_loopDumpState         = 0;   // 0 idle, 1 in-progress, 2 just-finished, 3 finished-with-errors
volatile unsigned g_loopDumpTracksWritten = 0;

static bool s_requested   = false;   // set by loopDumpRequest(), cleared once the dump actually starts
static bool s_active      = false;
static bool s_hadError    = false;
static int  s_trackIdx    = -1;      // track currently being written, -1 = between tracks
static FIL  s_file;
static bool s_fileOpen    = false;
static u32  s_blocksTotal = 0;
static u32  s_blocksDone  = 0;

static void ldumpWriteHeader (u32 dataBytes)
{
    u8 h[LDUMP_HDR_BYTES];
    u32 rate       = AUDIO_SAMPLE_RATE;
    u16 channels   = LOOPER_NUM_CHANNELS;
    u16 bits       = 16;
    u32 byteRate   = rate * channels * (bits / 8);
    u16 blockAlign = channels * (bits / 8);
    u32 riffLen    = 36 + dataBytes;
    u32 fmtLen     = 16;
    u16 pcm        = 1;
    memcpy (h,      "RIFF", 4);   memcpy (h + 4,  &riffLen, 4);
    memcpy (h + 8,  "WAVE", 4);   memcpy (h + 12, "fmt ", 4);
    memcpy (h + 16, &fmtLen, 4);  memcpy (h + 20, &pcm, 2);
    memcpy (h + 22, &channels, 2);memcpy (h + 24, &rate, 4);
    memcpy (h + 28, &byteRate, 4);memcpy (h + 32, &blockAlign, 2);
    memcpy (h + 34, &bits, 2);    memcpy (h + 36, "data", 4);
    memcpy (h + 40, &dataBytes, 4);
    UINT bw;
    f_lseek (&s_file, 0);
    f_write (&s_file, h, LDUMP_HDR_BYTES, &bw);
}

static void ldumpCloseCurrentFile (bool ok)
{
    if (s_fileOpen)
    {
        if (ok) ldumpWriteHeader (s_blocksTotal * AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS * (unsigned)sizeof(s16));
        f_close (&s_file);
        s_fileOpen = false;
    }
    if (!ok) s_hadError = true;
    s_trackIdx = -1;
    s_blocksTotal = 0;
    s_blocksDone = 0;
}

// Advance the currently-open track file by up to one chunk. Returns true when
// this track is fully written (file closed) so the caller can move to the next.
static bool ldumpAdvanceOneChunk (loopClip *pClip)
{
    u32 remainBlocks = s_blocksTotal - s_blocksDone;
    u32 chunkBlocks = remainBlocks < LDUMP_CHUNK_BLOCKS ? remainBlocks : LDUMP_CHUNK_BLOCKS;
    if (chunkBlocks == 0) { ldumpCloseCurrentFile (true); return true; }

    s16 *src = pClip->getBlock (s_blocksDone);
    UINT bw;
    UINT bytes = chunkBlocks * AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS * (UINT)sizeof(s16);
    if (f_write (&s_file, src, bytes, &bw) != FR_OK || bw != bytes)
    {
        ldumpCloseCurrentFile (false);
        return true;
    }
    s_blocksDone += chunkBlocks;
    if (s_blocksDone >= s_blocksTotal) { ldumpCloseCurrentFile (true); return true; }
    return false;
}

// Find the next track (>= startFrom) that has a recorded clip, open its WAV
// file, and leave it as the in-progress file. Returns the track index opened,
// or -1 if no more tracks have recorded content.
static int ldumpOpenNextTrack (int startFrom)
{
    for (int t = startFrom; t < LOOPER_NUM_TRACKS; t++)
    {
        loopTrack *pTrack = ((loopMachine*)pTheLoopMachine)->getTrack ((u16)t);
        if (!pTrack) continue;
        if (pTrack->getNumRecordedClips() == 0) continue;

        loopClip *pClip = pTrack->getClip (0);
        u32 numBlocks = pClip->getNumBlocks();
        if (numBlocks == 0) continue;

        char path[48];
        // "USB:/looper-trackNN.wav" -- NN zero-padded, stable/sortable names.
        strcpy (path, LDUMP_PATH_PREFIX);
        int len = (int)strlen (path);
        path[len]     = (char)('0' + (t / 10));
        path[len + 1] = (char)('0' + (t % 10));
        path[len + 2] = 0;
        strcat (path, ".wav");

        if (f_open (&s_file, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
            continue;                       // skip this track, try the next

        s_fileOpen    = true;
        s_blocksTotal = numBlocks;
        s_blocksDone  = 0;
        f_lseek (&s_file, LDUMP_HDR_BYTES);
        return t;
    }
    return -1;
}

void loopDumpInit (void)
{
    s_requested = false;
    s_active = false;
    s_hadError = false;
    s_trackIdx = -1;
    s_fileOpen = false;
    g_loopDumpState = 0;
    g_loopDumpTracksWritten = 0;
}

void loopDumpRequest (void)
{
    // Ignore a re-press while a dump is already running or awaiting the Core-2
    // tick to start -- one dump at a time, matching the momentary hold/press
    // command model everywhere else in the looper.
    if (s_active || s_requested) return;
    s_requested = true;
}

void loopDumpTick (void)
{
    if (!s_active)
    {
        if (!s_requested) return;
        s_requested = false;

        if (!g_uwavMounted)
        {
            // No drive mounted -- nothing to dump to. Signal "finished with
            // errors" so the pad still flashes (operator feedback that the
            // press was seen, even though there was no drive to write to).
            g_loopDumpState = 3;
            return;
        }

        s_hadError = false;
        g_loopDumpTracksWritten = 0;
        s_active = true;
        g_loopDumpState = 1;
        s_trackIdx = ldumpOpenNextTrack (0);
        if (s_trackIdx < 0)
        {
            // No recorded tracks at all -- nothing to write, finish immediately.
            s_active = false;
            g_loopDumpState = s_hadError ? 3 : 2;
            return;
        }
        return;
    }

    // A drive removal mid-dump aborts the current file write cleanly rather
    // than fault on a stale FIL handle.
    if (!g_uwavMounted)
    {
        ldumpCloseCurrentFile (false);
        s_active = false;
        g_loopDumpState = 3;
        return;
    }

    loopTrack *pTrack = ((loopMachine*)pTheLoopMachine)->getTrack ((u16)s_trackIdx);
    loopClip  *pClip  = pTrack ? pTrack->getClip (0) : 0;
    if (!pClip) { ldumpCloseCurrentFile (false); s_trackIdx = ldumpOpenNextTrack (s_trackIdx + 1); }
    else if (ldumpAdvanceOneChunk (pClip))
    {
        g_loopDumpTracksWritten++;
        s_trackIdx = ldumpOpenNextTrack (s_trackIdx + 1);
    }

    if (s_trackIdx < 0)
    {
        s_active = false;
        g_loopDumpState = s_hadError ? 3 : 2;
    }
}
