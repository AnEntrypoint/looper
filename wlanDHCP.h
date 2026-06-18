#ifndef _wlanDHCP_h
#define _wlanDHCP_h
#include <wlan/bcm4343.h>
#include <circle/types.h>
void wlanDhcpSendDiscover(CBcm4343Device *pWLAN, const u8 *mac);
bool wlanDhcpPoll(CBcm4343Device *pWLAN);   // drives DISCOVER retry; true when leased or capped
bool wlanDhcpOK(void);
int  wlanDhcpAttempts(void);                // DISCOVERs sent this burst (telemetry)
bool wlanDhcpFailed(void);                   // capped without a lease
unsigned wlanDhcpRxSeen(void);               // frames seen addressed to client :68
unsigned wlanDhcpOffersSeen(void);           // valid OFFERs parsed
const u8 *wlanDhcpIP(void);
void wlanApSetIP(const u8 *ip);
void wlanApInit(CBcm4343Device *pWLAN);

// Single-RX-demux handlers: each inspects ONE already-received frame and returns
// true iff it owned (consumed) it. The shared radio RX queue is drained in ONE
// place (abletonLink.cpp::linkProcess) and each frame is offered to these by
// port, so the DHCP and Link consumers no longer drop each other's packets.
bool wlanDhcpServeFrame(const u8 *buf, int len);   // AP: DHCP :67 DISCOVER/REQUEST
bool wlanDhcpClientFrame(const u8 *buf, int len);  // STA: DHCP :68 OFFER reply
#endif
