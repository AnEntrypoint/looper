#include "input_otg.h"
#include "usbaudiogadget.h"

extern void AudioInputUSB_injectOTG (const s16 *pLeft, const s16 *pRight, unsigned nSamples);

void AudioInputOTG::start (void)
{
	CUSBAudioGadget *pGadget = CUSBAudioGadget::Get ();
	if (pGadget)
		pGadget->RegisterOutHandler (injectHandler);
}

void AudioInputOTG::injectHandler (const s16 *pLeft, const s16 *pRight, unsigned nSamples)
{
	AudioInputUSB_injectOTG (pLeft, pRight, nSamples);
}
