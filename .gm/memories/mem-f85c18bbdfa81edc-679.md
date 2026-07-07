---
key: mem-f85c18bbdfa81edc-679
ns: default
created: 1783164598284
updated: 1783164598284
---

gm-method: a wire-protocol field that's encoded but hardcoded to a constant (e.g. always 0) with a comment like 'required by X's parser' is a strong signal of dead/unimplemented functionality worth grepping for callers before assuming it's fully wired -- in this session lwAppendStartStop existed and was called, but with isPlaying always 0, because the real transport-state logic to drive it had never been added. Finding one such hardcoded-placeholder call site is worth a broader grep for the same pattern (search for other constant/zero args passed to protocol encoders) since it often means an entire feature direction (send AND receive) is unimplemented, not just one line.
