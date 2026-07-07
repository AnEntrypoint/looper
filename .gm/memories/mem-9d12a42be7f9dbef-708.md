---
key: mem-9d12a42be7f9dbef-708
ns: default
created: 1779962038252
updated: 1779962038252
---

## Resolved mutable: worst-case-press-process-latency

Bounded by ONE Core-2 coreControlPlaneTick iteration (kernel_run.cpp:34: USBHCI PnP + Net.Process + usbMidiProcess + loop()[drains apcKey25 cmd ring] + wlanDhcp + linkProcess + Scheduler.Yield). MIDI IN is ISR-immediate (usbMidi packetHandler); the press is acted on at the next loop() within a tick. Net.Process/linkProcess can make a tick take up to a few ms under load; not block-locked. So press->process is small (sub-ms typical) but JITTERY — exactly why ISR-timestamping the press + backdating is needed (press-timestamp-isr). The 10.6ms IN-ring history covers the realistic worst case with margin; clamp+flag beyond (backdate-exceeds-history).
