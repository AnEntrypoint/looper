---
key: mem-3be9afadebb4ba2f-792
ns: default
created: 1779799393864
updated: 1779799393864
---

## Resolved mutable: lpc-formant-erratic-on-real-pi-input

RESOLVED on the genuine LPC build. /tmp/capspec.exe on Pi loopback (196Hz, -12): formant down=27 center=21 up=31 clicks vs DRY-passthrough floor=30 clicks. The formant captures are AT OR BELOW the rig click floor = the LPC formant adds ZERO clicks beyond the noisy loopback rig (unlike the OLD pre-resample build which spiked down=81/up=34 on top of the floor). The earlier erratic hardware numbers were from the STALE pre-resample kernel (LPC never built). Centroid direction unreliable on the rig (dry reads 1331Hz, rig coloration dominates) but host test-formant-lpc proved monotonic envelope shift + exact pitch. -12 stays exact, no emergencies, CPU healthy. Artifact-free on hardware; perceptual direction is the user ear-check.
