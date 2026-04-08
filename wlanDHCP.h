#ifndef _wlanDHCP_h
#define _wlanDHCP_h
#include <wlan/bcm4343.h>
#include <circle/types.h>
void wlanDhcpGetIP(CBcm4343Device *pWLAN, const u8 *mac);
bool wlanDhcpOK(void);
const u8 *wlanDhcpIP(void);
#endif
