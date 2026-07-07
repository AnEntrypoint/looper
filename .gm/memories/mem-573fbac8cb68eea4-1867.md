---
key: mem-573fbac8cb68eea4-1867
ns: default
created: 1782066279185
updated: 1782066279185
---

## Looper WiFi + Ableton Link (opt-in LOOPER_ENABLE_WLAN, abletonLink.cpp)

WLAN+Link compile-gated OFF by default (#ifdef LOOPER_ENABLE_WLAN in patches/kernel.cpp); default build = wlan=disabled. FIRST check when 'no Link/no ticker'. Build scripts/build-local.ps1 -Wlan -> kernel7l-wlan.img. Ethernet = boot/syslog (static 192.168.137.100). WiFi (CBcm4343Device) Link-only raw SendFrame/ReceiveFrame, no CNetSubSystem. JoinOpenNet('ticker') first (3x retry); on fail CreateOpenNet hosts own AP (192.168.4.1, bare-metal DHCP wlanDHCPServer.cpp). WLAN bring-up DEFERRED off boot path (wlanServiceBringUp from coreControlPlaneTick after sockets bound). Full Link: 3 host-tested header modules linkWire.h (bit-exact wire codec, discovery _asdp_v 224.76.78.75:20808, measurement _link_v unicast, keys tmln/sess/mep4), linkGhost.h (GhostXForm, measurement accumulator, ghost timeline math), linkSession.h (peer table, AP learns MAC from Eth source no ARP, ownership=highest NodeId). SINGLE radio-RX demux: linkProcess() is SOLE drainer, routes by port (Link 20808, DHCP 67/68, LCLK 20810, LTMP 20811). UNICAST-RX WALL (HW-confirmed): bcm4343 delivers multicast/broadcast but NOT unicast-to-self (uniRx stays 0) -> standard Link measurement never completes; FIX = esp ticker multicasts LCLK clock (port 20810, 'LCLK'+i64) consumed as owner ghost offset (handleClockBroadcast; MUST parse BEFORE the plen<20 header guard since LCLK is 12 bytes). LOOPER SETS GROUP TEMPO+PHRASE: first loop multicasts LTMP (port 20811, microsPerBeat+espBeat0+quantum) ONE burst of 4 (continuous forceBeatAtTime jerked the esp). DHCP needs BOOTP broadcast flag. Symmetric topology wlanApYieldTry. :4445 WLAN/LINK/LMSG verbs. HW: WLAN-enabled kernel previously HANGED at boot before network (radio bring-up) -- HW-RE-VALIDATION PENDING before flipping default. See [[looper-audio-architecture]].
