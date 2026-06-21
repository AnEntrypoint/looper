#include "Looper.h"
#include "usbWavRecorder.h"
#include "continuousBuffer.h"
#include <ff.h>
#include <circle/timer.h>
#include <circle/util.h>

// One stereo s16 block (interleaved L,R) = the continuousBuffer block layout.
#define UWAV_BLOCK_BYTES   (AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS * (unsigned)sizeof(s16))
#define UWAV_CHUNK_BLOCKS  64                                   // ~16KB write granularity
#define UWAV_CHUNK_BYTES   (UWAV_CHUNK_BLOCKS * UWAV_BLOCK_BYTES)
#define UWAV_HDR_BYTES     44
#define UWAV_PATH          "USB:/looper-rec.wav"
#define UWAV_RETRY_US      1000000ull                          // probe for a drive ~1Hz
#define UWAV_SYNC_US       2000000ull                          // flush FatFs cache ~0.5Hz
#define UWAV_MAX_CHUNKS_PER_TICK 8                             // burst limiter (Core-2 safe)

volatile unsigned           g_uwavMounted = 0;
volatile unsigned           g_uwavWraps   = 0;
volatile unsigned long long g_uwavBytes   = 0;
volatile unsigned long long g_uwavMaxData = 0;

static FATFS s_fs;
static FIL   s_file;
static bool  s_mounted = false;
static bool  s_open    = false;
static u32   s_lastBlock   = 0;       // next cb block to drain
static u64   s_dataCursor  = 0;       // bytes written into the data region [0, s_maxData)
static u64   s_maxData     = 0;       // ring data size (bytes)
static u64   s_lastMountTry = 0;
static u64   s_lastSync     = 0;

// 16KB staging for one chunk copied out of the cb (handles the cb wrap internally).
static s16   s_chunk[UWAV_CHUNK_BLOCKS * AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS];

static void uwavWriteHeader (u32 dataBytes)
{
	u8 h[UWAV_HDR_BYTES];
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
	f_write (&s_file, h, UWAV_HDR_BYTES, &bw);
}

static void uwavClose (void)
{
	if (s_open)    { f_close (&s_file); s_open = false; }
	if (s_mounted) { f_mount (0, "USB:", 0); s_mounted = false; }
	g_uwavMounted = 0;
}

// Probe for a drive: mount the USB volume, size the ring to (free space - margin)
// clamped under the FAT32 4GB file limit, create the WAV + header. Cheap + quick to
// fail (FR_NOT_READY) when no drive is present, so safe to call ~1Hz on Core 2.
static bool uwavTryOpen (void)
{
	if (f_mount (&s_fs, "USB:", 1) != FR_OK) return false;
	s_mounted = true;

	u64 maxData = 256ull * 1024 * 1024;                 // fallback 256MB
	DWORD freeClust = 0; FATFS *pfs = 0;
	if (f_getfree ("USB:", &freeClust, &pfs) == FR_OK && pfs)
	{
		u64 freeBytes = (u64) freeClust * pfs->csize * 512ull;
		if (freeBytes > 64ull * 1024 * 1024) maxData = freeBytes - 32ull * 1024 * 1024;
	}
	const u64 fat32cap = 0xFFFFFFFFull - 1024ull * 1024;  // keep under 4GB
	if (maxData > fat32cap) maxData = fat32cap;
	maxData -= maxData % UWAV_BLOCK_BYTES;                 // whole stereo frames
	if (maxData < UWAV_CHUNK_BYTES) { uwavClose (); return false; }

	if (f_open (&s_file, UWAV_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { uwavClose (); return false; }
	s_maxData     = maxData;
	g_uwavMaxData = maxData;
	uwavWriteHeader ((u32) maxData);
	f_lseek (&s_file, UWAV_HDR_BYTES);
	s_dataCursor = 0;
	s_lastBlock  = g_cbWriteBlock;                         // start from "now"
	s_open       = true;
	g_uwavMounted = 1;
	return true;
}

void usbWavInit (void)
{
	s_mounted = false; s_open = false;
	s_dataCursor = 0; s_maxData = 0;
}

void usbWavTick (void)
{
	u64 now = CTimer::GetClockTicks ();

	if (!s_open)
	{
		if (now - s_lastMountTry < UWAV_RETRY_US) return;
		s_lastMountTry = now;
		uwavTryOpen ();
		return;
	}

	u32 wr = g_cbWriteBlock;
	// Overrun guard: if Core 2 fell so far behind that the cb wrapped past our read
	// cursor, the oldest audio is gone -- fast-forward to the live horizon.
	u32 avail = cbBlocksAvailable ();
	if ((u32) (wr - s_lastBlock) > avail)
		s_lastBlock = wr - (avail > 4 ? avail - 4 : 0);

	int chunks = 0;
	while ((u32) (wr - s_lastBlock) >= UWAV_CHUNK_BLOCKS && chunks < UWAV_MAX_CHUNKS_PER_TICK)
	{
		cbCopyRange (s_lastBlock, s_lastBlock + UWAV_CHUNK_BLOCKS, s_chunk);

		u64   remain = s_maxData - s_dataCursor;
		UINT  bw;
		if ((u64) UWAV_CHUNK_BYTES <= remain)
		{
			if (f_write (&s_file, s_chunk, UWAV_CHUNK_BYTES, &bw) != FR_OK || bw != UWAV_CHUNK_BYTES)
				{ uwavClose (); return; }
			s_dataCursor += UWAV_CHUNK_BYTES;
			if (s_dataCursor >= s_maxData)            // exact fill -> wrap to data start
				{ f_lseek (&s_file, UWAV_HDR_BYTES); s_dataCursor = 0; g_uwavWraps++; }
		}
		else
		{
			// Chunk straddles the ring end: write the tail, wrap, write the rest.
			if (remain > 0)
				if (f_write (&s_file, s_chunk, (UINT) remain, &bw) != FR_OK) { uwavClose (); return; }
			f_lseek (&s_file, UWAV_HDR_BYTES);
			UINT rest = (UINT) (UWAV_CHUNK_BYTES - remain);
			if (f_write (&s_file, s_chunk + (remain / sizeof (s16)), rest, &bw) != FR_OK) { uwavClose (); return; }
			s_dataCursor = rest;
			g_uwavWraps++;
		}

		s_lastBlock  += UWAV_CHUNK_BLOCKS;
		g_uwavBytes  += UWAV_CHUNK_BYTES;
		chunks++;
	}

	if (now - s_lastSync >= UWAV_SYNC_US) { f_sync (&s_file); s_lastSync = now; }
}
