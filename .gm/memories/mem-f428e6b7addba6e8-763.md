---
key: mem-f428e6b7addba6e8-763
ns: default
created: 1783157098358
updated: 1783157098358
---

gm-method: when a bug report claims 'state X should reset on condition Y but doesn't', don't stop at the first plausible reset-path (e.g. an erase/clear handler) even if it looks buggy on inspection -- verify by reading the ACTUAL current code with codesearch before writing a fix, since an earlier commit may have already fixed that path. In this session the erase-path masterLoopBlocks reset was already correct (an anyClips scan over all tracks); the real bug was a DIFFERENT code path (a first-record/external-sync branch) that silently deferred grid-definition to a downstream re-derivation with no valid anchor yet. Two codesearch passes (initial hypothesis, then re-verify against the full call site) caught the wrong-hypothesis before a wasted fix landed.
