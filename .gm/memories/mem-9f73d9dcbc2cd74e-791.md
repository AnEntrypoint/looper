---
key: mem-9f73d9dcbc2cd74e-791
ns: default
created: 1780654718044
updated: 1780654718044
---

## Looper arrangement memory tracks looper erase (commit follows)

A preset IS an arrangement: m_presetMask[p] = the set of loopers it is made of. When a looper is erased (long-hold ERASE_TRACK, apcKey25.cpp), apcKey25::_forgetLooperFromPresets(n) (apcKey25Notes.cpp) drops bit n from EVERY mask; any arrangement whose mask reaches 0 is deleted (m_presetUsed[p]=false) so _updateGridLeds (apcKey25Transpose.cpp:124) draws its pad OFF -> the arrangement light goes dark exactly when its last member is gone. CLEAR_ALL (shift+PLAY, LOOP_COMMAND_CLEAR_ALL=0x01) calls _forgetAllPresets(). Erasing empty/non-member looper is a no-op (only a set bit cleared, delete only on mask->0); deleted slot reusable by fresh _capturePreset. Witnessed scripts/test-arrangement-forget.cpp 17 checks ALL PASS.
