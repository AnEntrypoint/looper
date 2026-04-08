#ifndef _abletonLink_h
#define _abletonLink_h

#include <wlan/bcm4343.h>
#include <circle/types.h>

void linkInit(CBcm4343Device *pWLAN);
void linkProcess(void);
double linkGetBPM(void);
void linkSetBPM(double bpm);
bool linkIsSynced(void);

#endif
