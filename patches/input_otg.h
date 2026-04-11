#ifndef _input_otg_h_
#define _input_otg_h_

#include <circle/types.h>

class AudioInputOTG
{
public:
	void start (void);
	static void injectHandler (const s16 *pLeft, const s16 *pRight, unsigned nSamples);
};

#endif
