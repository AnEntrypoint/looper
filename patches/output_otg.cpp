#include "output_otg.h"
#include "usbaudiogadget.h"

extern void AudioOutputUSB_tapOTG (s16 *pLeft, s16 *pRight, unsigned nSamples);

void AudioOutputOTG::start (void)
{
	CUSBAudioGadget *pGadget = CUSBAudioGadget::Get ();
	if (pGadget)
		pGadget->RegisterInHandler (tapHandler);
}

void AudioOutputOTG::tapHandler (s16 *pLeft, s16 *pRight, unsigned nSamples)
{
	AudioOutputUSB_tapOTG (pLeft, pRight, nSamples);
}
