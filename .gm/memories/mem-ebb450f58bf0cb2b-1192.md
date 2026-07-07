---
key: mem-ebb450f58bf0cb2b-1192
ns: default
created: 1779909968106
updated: 1779909968106
---

{"text":"## LED consistency on APC Key25 (looper)

Three fixes (commit aef4200): (1) recording state = APC_VEL_LED_RED_BLINK not solid RED (apcKey25Transpose.cpp:145) — user wants red-flash while recording. APC hardware self-blinks on one NoteOn at a *_BLINK velocity, so coalescing a steady blink is safe (no re-trigger needed). (2) Live-engage LED (note 0x40) was fire-and-forget usbMidiSend that cleared its dirty flag unconditionally -> dropped frame stranded the LED stale; now usbMidiSendNoteOn (bool) clears dirty only on success = retries next tick. (3) usbMidiProcess re-queries every MIDI slot each PnP update and tracks the live device pointer (was sticky s_registered never cleared on disconnect) -> invalidateLedCache on ANY roster change so post-reconnect LEDs re-sync. All grid/preset/VU/transport LEDs already go through sendLedCoalesced (retry-on-drop cache, commits cache only on send success). _sendLed is dead code (zero callers). scripts/test-led-coalesce.cpp: 8/8 host assertions prove the retry-cache invariant — drop leaves cache unchanged -> retries, sustained drops never lose state, invalidateLedCache re-sends same value.","tags":["looper","apc","led","midi"]}
