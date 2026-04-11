#ifndef _output_otg_h_
#define _output_otg_h_

#include <circle/types.h>

class AudioOutputOTG
{
public:
	void start (void);
	static void tapHandler (s16 *pLeft, s16 *pRight, unsigned nSamples);
};

#endif
