---
key: mem-ccb0e1cecde75324-1232
ns: default
created: 1779790257607
updated: 1779790257607
---

## Resolved mutable: formant-not-shifting-envelope

CONFIRMED + research done. Centroid non-monotonic across depths (519/417/492/376/424Hz) = pre-resample-before-pitch CANNOT shift formants independently (two cascaded resampling reads on one delay line move pitch+envelope together; my scale/preRate compensation cancels the envelope shift, leaving only doubling artifacts). User chose option 2 (real envelope warp) with HARD constraint: zero added latency. Web/arxiv research (HiFi-Glot 2409.14823, diff-TVLP 2406.05128, LPCNet) all converge on the SAME classical zero-latency core: TIME-VARYING LPC SOURCE-FILTER. Algorithm (textbook Markel&Gray / Rabiner&Schafer, all causal=zero latency): (1) LPC analysis (autocorrelation, order ~12) on a trailing past-only window recomputed per ~5-10ms block, coeffs smoothed to sample rate; (2) inverse filter e(t)=x(t)+sum a_i x(t-i) extracts residual (pitch/excitation); (3) formant shift = warp the LPC pole frequencies (scale envelope) -> new synth filter a_prime; (4) resynth y(t)=e(t)-sum a_prime_i y(t-i). Pitch lives in residual (untouched), only envelope moves, composes with -12 WITHOUT cancelling. Zero added latency (analysis is causal). Replaces the pre-resample stage entirely.
