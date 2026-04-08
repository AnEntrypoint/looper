#ifndef _wlanDHCP_h
#define _wlanDHCP_h
#include <wlan/bcm4343.h>
#include <circle/types.h>
void wlanDhcpSendDiscover(CBcm4343Device *pWLAN, const u8 *mac);
bool wlanDhcpPoll(CBcm4343Device *pWLAN);
bool wlanDhcpOK(void);
const u8 *wlanDhcpIP(void);
#endif
