---
key: mem-ab3edce5c1fbdf2e-1449
ns: default
created: 1781812521669
updated: 1781812521669
---

## APC LEDs go dark when a second stuck MIDI device shares the GLOBAL OUT in-flight cap (commit de48556)

plugging a Tascam US-2x2 (UAC2 + USB-MIDI) alongside the APC Key 25 made the APC LEDs go dark. The APC enumerated fine (:4445 MIDI midiDevices=2 slots=0x03 = umidi1+umidi2 both present) but every LED send dropped (outDrop ran to 139384+ climbing). ROOT CAUSE: patches/usbmidihost.cpp capped concurrent OUT URBs with a SINGLE GLOBAL counter s_MIDIOutInFlight (USBMIDI_OUT_MAX_INFLIGHT=1) shared across ALL MIDI devices; the US-2x2's USB-MIDI OUT endpoint never drains so its URB pinned the global at 1 forever -> every APC send hit the cap and dropped. FIX: cap PER-OWNER via countOwnerInFlight(pOwner) (count busy s_MIDIOutSlots for that device) in SendEventsHandler, so a wedged device starves only itself. usbMidiSendNoteOn still broadcasts LEDs to all umidi1..8 (the non-APC device's drops just plateau, harmless). LIVE: outDrop 139384-climbing -> 2302-stable, APC LEDs lit. DIAGNOSED with a new :4445 MIDI verb (usbMidi.cpp usbMidiTelemetry + g_midiInPackets): midiDevices/slots(bitmask of umidi1..8)/inPkts/outDrop/outErr -- slots=0 would mean not-enumerated, outDrop-climbing means LED sends dropping. The kernel runs NO syslog daemon (m_pSysLog=nullptr) so :4445 verbs are the only live observability. Whenever 'APC dark / LEDs frozen' with another USB-MIDI device present, suspect shared MIDI-OUT resource starvation, check :4445 MIDI.
