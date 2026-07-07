---
key: mem-6851f7bdcfbe4af4-490
ns: default
created: 1782806432618
updated: 1782806432618
---

## Resolved mutable: slot-busy-race

patches/usbmidihost.cpp:195-207 MIDIOutCompletion only accesses pSlot (the TMIDIOutSlot* passed as pContext -- a pointer to a static global array element). It sets pSlot->bBusy=FALSE and decrements s_MIDIOutInFlight. It never reads or dereferences pSlot->pOwner. Therefore clearing pOwner=nullptr in the destructor while a URB is still in-flight is safe -- the completion will fire, set bBusy=FALSE, and return normally. No use-after-free. Race is safe.
