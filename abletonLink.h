#ifndef _abletonLink_h
#define _abletonLink_h

#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/types.h>

void linkInit(CNetSubSystem *pNet, CSocket *pSocket);
void linkProcess(void);
double linkGetBPM(void);
void linkSetBPM(double bpm);
bool linkIsSynced(void);

#endif
