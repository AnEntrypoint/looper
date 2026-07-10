---
key: mem-3c9dcefeaaa01247-1951
ns: default
created: 1783686310758
updated: 1783686310758
---

## Looper Pi4 build uses XHCI, not DWHCI -- verify driver attribution before analyzing lib/usb/dwhci*.cpp

RASPPI=4 (this project's only target) uses XHCI (USB 3.0 host controller, lib/usb/xhci*.cpp) not DWHCI (USB 2.0, older Pi models, lib/usb/dwhci*.cpp). Confirmed via arm-none-eabi-nm on the actual built kernel7l.elf: CDWHCITransferStageData symbols (including GetResultLen) are entirely absent from the link; CXHCIDevice symbols are present. Six-plus commits across a multi-session USB-audio buzz investigation (2026-07-08c through j) analyzed dwhcixferstagedata.cpp's TransactionComplete/GetResultLen mechanics as root cause for a UAC2 multi-packet iso URB result-length bug -- that analysis was CORRECT for DWHCI (verified line-by-line against real Circle source) but the file was never compiled into any deployed build on this hardware, so the root-cause narrative described dead code the whole time.

The REAL analog on THIS hardware's actual XHCI path: lib/usb/xhciendpoint.cpp CXHCIEndpoint::TransferEvent/EnqueueTRB. A multi-packet iso URB's N packets are independent TDs (no chain bit set -- SIA flag used instead), with IOC (interrupt-on-completion) set ONLY on the last packet's TRB, so exactly ONE XHCI transfer event fires per URB, reporting hardware residual length for ONLY that final packet. SetResultLen(nBufLen - lastResidual) silently assumes every packet before the last delivered its full declared size -- an OVER-report risk (an early short packet is invisible), the OPPOSITE failure shape from DWHCI's under-report/truncation.

Before citing/analyzing/patching ANY lib/usb/dwhci*.cpp file in this project: (1) confirm via arm-none-eabi-nm on the actual built kernel7l.elf whether the relevant DWHCI symbols are even linked, (2) if not, the real driver path is lib/usb/xhci*.cpp -- read/patch there instead. patches/usbaudiodevice.cpp's InCompletion comments (as of commit 4f05154) now correctly describe the XHCI mechanism.
