---
key: mem-fe52afce931cce92-1731
ns: default
created: 1783628955041
updated: 1783628955041
---

## Resolved mutable: why-existing-ring-drift-correction-doesnt-absorb-uac2-batching-error

Confirmed via re-reading dwhcixferstagedata.cpp TransactionComplete/GetResultLen: GetResultLength() for a multi-packet URB is a MONOTONIC UNDER-estimate (clamped down to m_nTransferSize, which only ever shrinks the reported total, never inflates it -- m_nTotalBytesTransfered is the real accumulated received-byte count, and GetResultLen() returns min(m_nTotalBytesTransfered, m_nTransferSize)). This means GetResultLength() NEVER over-reports for UAC2 multi-packet -- it under-reports, but the bytes it DOES report are real, valid, correctly-positioned data (m_pBufferPointer advances by the true nBytesTransfered each packet, independent of what GetResultLen later reports). CONCLUSION: nSamples should be computed as min(pURB->GetResultLength()/frameBytes, m_nInSubmitBytes[slot]/frameBytes) -- take the SMALLER of the two candidates. GetResultLength() gives a safe (possibly under-counted but never garbage) real value; m_nInSubmitBytes gives the upper bound of what COULD have been received. Taking the min guarantees InCompletion never reads past real data (eliminates the over-read/garbage-read bug that caused the buzz) while still using GetResultLength() as often as it's accurate (single-packet URBs, or multi-packet URBs where the clamp coincidentally doesn't engage). This mirrors the UAC1 fix's principle (trust the real measured value) while respecting that GetResultLength() alone under-delivers throughput on the multi-packet path -- capping via min() is strictly safer than either value alone: never over-reads (unlike constant m_nInSubmitBytes), and never crashes/asserts (GetResultLength() is still gated on GetStatus()).
