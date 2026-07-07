---
key: mem-fc8f2cade54a7f05-770
ns: default
created: 1781692070977
updated: 1781692070977
---

## Resolved mutable: link-mep4-addr-order

Convergent source evidence: (1) anweiss/ableton-link-rs src/encoding.rs Ipv4Addr encode_to = out.extend_from_slice(&self.octets()), encoded_size=4 (4 bytes network order, as-is); u16 = 2 bytes big-endian. (2) Ableton/link PeerState.hpp: MeasurementEndpointV4 wraps an asio::ip::udp::endpoint, key kMep4=0x6d657034; asio endpoint canonical serialization = address THEN port. (3) The key is family-specific (mep4 vs mep6) so the value carries NO family prefix. => mep4 value = addr(4 octets, network order) + port(2, BE) = 6 bytes, which is exactly what linkWire.h lwAppendMep4 emits (v[0..3]=addr, lwPut16(v,4,port)). Residual final byte-order confirmation against a live capture is covered by the external link-hw-validate row.
