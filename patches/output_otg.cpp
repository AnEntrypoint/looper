#include "output_otg.h"
#include "usbaudiogadget.h"
#include <circle/util.h>

extern void AudioOutputUSB_tapOTG (s16 *pLeft, s16 *pRight, unsigned nSamples);

// Test sine: 440Hz at 48000Hz sample rate
// sin table: 109 samples per cycle (48000/440 = 109.09)
#define SINE_FREQ_HZ    440
#define SINE_SAMPLE_RATE 48000
#define SINE_AMPLITUDE  16000

static unsigned s_sinePhase = 0;

// integer sine approximation: Bhaskara I
// returns sin(phase/65536 * 2pi) * amplitude, phase in [0,65536)
static s16 sineApprox (unsigned phase, s16 amplitude)
{
	// Map phase [0,65536) to [0,65536) representing [0,2pi)
	unsigned p = phase & 0xFFFF;
	// Reflect into [0, 32768) = [0, pi)
	unsigned half = (p < 32768) ? p : 65536 - p;
	// Bhaskara: sin(x) ≈ 16*x*(pi-x) / (5*pi^2 - 4*x*(pi-x))
	// With x in [0,32768] representing [0,pi], normalized to integer:
	// Using fixed-point: let x = half, pi = 32768
	// sin ≈ (4*x*(pi-x)) / (pi^2 - x*(pi-x)) * ... simplified to:
	// sin ≈ (4 * half * (32768 - half)) / (32768*32768 - half*(32768-half)) * 2^15... too complex
	// Simple lookup: compute via phase increment
	// Use Q15 fixed-point: sin(2pi * phase/65536)
	// Parabolic approx: sin(x) ≈ 4x(pi-x)/pi^2 for x in [0,pi]
	// half in [0,32768], let h = half << 1 (maps to [0,65536])
	// y = h * (65536 - h) >> 15 gives parabola peaking at 32768 at h=32768
	unsigned h = (unsigned)(half) * 2;
	s32 y = (s32)h * (s32)(65536 - h);
	y >>= 15; // y in [0, 32768] approximately
	if (p >= 32768) y = -y;
	return (s16)((s32)y * amplitude >> 15);
}

void AudioOutputOTG::start (void)
{
	CUSBAudioGadget *pGadget = CUSBAudioGadget::Get ();
	if (pGadget)
		pGadget->RegisterInHandler (tapHandler);
}

void AudioOutputOTG::tapHandler (s16 *pLeft, s16 *pRight, unsigned nSamples)
{
	AudioOutputUSB_tapOTG (pLeft, pRight, nSamples);

	// If output is silent (all zeros), overlay 440Hz test tone so we can
	// verify the OTG pipeline works independently of UCA222 audio.
	bool allZero = true;
	for (unsigned i = 0; i < nSamples && allZero; i++)
		if (pLeft[i] || pRight[i]) allZero = false;

	if (allZero)
	{
		// Phase increment per sample: 65536 * SINE_FREQ_HZ / SINE_SAMPLE_RATE
		const unsigned inc = (unsigned)((u64)65536 * SINE_FREQ_HZ / SINE_SAMPLE_RATE);
		for (unsigned i = 0; i < nSamples; i++)
		{
			s16 v = sineApprox (s_sinePhase, SINE_AMPLITUDE);
			pLeft[i]  = v;
			pRight[i] = v;
			s_sinePhase += inc;
		}
	}
}
