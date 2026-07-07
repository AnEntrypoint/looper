---
key: mem-4c51f9c9bd1feee2-971
ns: default
created: 1779718621672
updated: 1779718621672
---

## Resolved mutable: snac-never-locks-on-pi-only

FIXED — SNAC now locks on the Pi. HDMI pk-eng.png: lock=1, peakV=999 (perfect SNAC peak), peakTau=436 (correct period for 110Hz), period=435/436, gap bounded 809-2198 (small, no runup), emerg+0/+1 (escapes nearly gone), splice 5-13/2s (normal). The seed-period-on-engage + faster incremental sweep made SNAC acquire and hold lock reliably. The engine is now internally CORRECT: locked, tracking the right period, bounded gap, scale=0.500. RESIDUAL: the captured -12 OUTPUT still wobbles 50-58Hz around ~53 despite the perfect internal state. Since the engine telemetry is now ideal, the wobble is downstream (OUT/OTG path) or measurement (host capture clock beat) — though DRY passthrough through the same path/capture reads steady 110.00. Next: determine if the output wobble is periodic (real, engine/ring rate) vs random (measurement) via fine peak-freq stats, and whether the ±0.39% ring clamp actually deployed.
