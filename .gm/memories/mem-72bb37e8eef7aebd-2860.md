---
key: mem-72bb37e8eef7aebd-2860
ns: default
created: 1781693576292
updated: 1781693576292
---

## FULL Ableton Link sync over the WiFi ticker AP (commit ef5deaa)

Three pure host-tested header modules (compiled into firmware AND tests): linkWire.h (bit-exact codec: discovery _asdp_v ALIVE/RESPONSE/BYEBYE mcast 224.76.78.75:20808 + measurement _link_v PING/PONG unicast; entry=key(4BE)+size(4BE)+value, ints BE, micros i64; keys tmln/sess/mep4 + __ht/__gt/_pgt; tmln=microsPerBeat(i64)+beatOrigin(MICROBEATS,i64)+timeOrigin(i64); mep4=IPv4(4)+port(2 BE) confirmed vs Rust reimpl+reference+real-Live Wireshark dissector offsets 28/36/44/52/60). linkGhost.h (GhostXForm{slope=1,offset=median(samples)}; host<->ghost=host+offset; measurement accumulator done@100/exhausted@5; ghost timeline math microbeats(ghost)=beatOrigin+(ghost-timeOrigin)*1e6/mpb; sample s1=ghost-(host+prevHost)/2,s2=(ghost+prevGhost)/2-prevHost). linkSession.h (peer table NodeId->{MAC,ipv4,mep4,timeline,meas,xform,lastSeen}; AP-mode learns MAC from Ethernet SOURCE => NO ARP; owner=highest NodeId with timeline; expiry 5s + hand-back; capacity 8; self-ignore).

Firmware abletonLink.cpp (Core 2 linkProcess): sendDiscovery ALIVE(mep4=ourIP:20808,sess,tmln) 1Hz + RESPONSE to ALIVE + BYEBYE; single RX demux routes mcast _asdp_v + unicast _link_v(to ourIP:OUR_MEP4_PORT=20808) + DHCP :67/:68; PING responder (PONG sess+ghost=hostToGhost(s_ownXform,now)+prevGhost+echoed) reactive post-RX; measurement initiator driveMeasurement (per-peer ping burst @50ms x5 -> lwMeasurementSamples -> median -> peer.xform); republishTimeline adopts owner timeline (s_ownXform=owner.xform when following, identity when self-owns). handleMessage ignores own NodeId. TX (beacon+pings+igmp) gated on IsLinkUp; reactive pongs post-RX. nowMicros=CTimer::GetClockTicks()=microseconds.

masterPhase phase-sync (SAFE, loopMachine.cpp): paramSnapshot.h LiveParams += linkPhaseValid/linkBeatPhaseMicroBeats/linkQuantumMicroBeats; apcKey25::update publishes via linkGhostPhase(); loopMachine, when synced+phaseValid+IDLE(no running clips), tracks session sub-beat phase into m_masterPhase (blocksPerBeat from Live bpm) so the FIRST recorded loop downbeat lands on Live beat (whole looper inherits via that loop). NEVER mutates running-loop phase; does NOT pre-set m_masterLoopBlocks (first loop defines phrase; synced+anyRecorded branch time-stretches to Live tempo). :4445 LINK verb: synced/peers/owner/offsetUs/pingsTx/pongsRx/phaseValid/beatPhase100/bpm.

Still compile-gated LOOPER_ENABLE_WLAN (opt-in). Both kernels: dist/kernel7l.img safe default 1072532B (WLAN off, netboot), dist/kernel7l-wlan.img 1073108B (enabled). HARDWARE-VALIDATION PENDING (link-hw-validate, external): flash kernel7l-wlan.img, join ticker from a laptop running Live, watch :4445 LINK peers>=1/offsetUs-stable/phaseValid=1, ear-check phase lock. Host tests: test-link-wire/ghost/session/wlan-rxdemux + full looper suite ALL PASS.
