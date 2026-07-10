#ifndef _usbaudiodevice_h
#define _usbaudiodevice_h

#include <circle/usb/usbfunction.h>
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbrequest.h>
#include <circle/synchronize.h>
#include <circle/types.h>

#define USB_AUDIO_BLOCK_BYTES   256
#define USB_AUDIO_OUTBUF_BYTES  512    // holds N=8 microframes/URB for UAC2 multi-packet OUT
                                        // (OUT self-throttles bytes/packet to fit, host-paced)
#define USB_AUDIO_INBUF_BYTES   2048   // sized to hold N=8 FULL microframes/URB at the device's
                                        // actual declared max packet size (up to 256B/microframe --
                                        // covers e.g. 4ch x 24-bit x ~21 samples/microframe for a
                                        // high-channel-count device like the AIR 192|4). IN is
                                        // device-paced: unlike OUT, a packet MUST be large enough
                                        // for whatever the device actually sends, or the transfer
                                        // truncates/errors -- so this buffer is sized up rather
                                        // than the per-packet byte budget shrunk down.

typedef void TAudioInHandler  (const s16 *pLeft, const s16 *pRight, unsigned nSamples);
typedef void TAudioOutHandler (s16 *pLeft, s16 *pRight, unsigned nSamples);

class CUSBAudioDevice : public CUSBFunction
{
public:
    CUSBAudioDevice (CUSBFunction *pFunction);
    ~CUSBAudioDevice (void);

    boolean Configure (void);

    void RegisterInHandler  (TAudioInHandler  *pHandler);
    void RegisterOutHandler (TAudioOutHandler *pHandler);

    static CUSBAudioDevice *Get (void) { return s_pThis; }
    static CUSBAudioDevice *GetOut (void) { return s_pOut; }

private:
    boolean StartInRequest  (unsigned slot = 0);
    boolean StartOutRequest (unsigned slot = 0);
    boolean StartFbRequest  (void);

    void InCompletion  (CUSBRequest *pURB);
    void OutCompletion (CUSBRequest *pURB);
    void FbCompletion  (CUSBRequest *pURB);

    static void InStub  (CUSBRequest *pURB, void *pParam, void *pContext);
    static void OutStub (CUSBRequest *pURB, void *pParam, void *pContext);
    static void FbStub  (CUSBRequest *pURB, void *pParam, void *pContext);

    CUSBEndpoint *m_pEndpointIn;
    CUSBEndpoint *m_pEndpointOut;
    CUSBEndpoint *m_pEndpointFb;    // UAC2 async OUT explicit feedback (IN dir)

    TAudioInHandler  *m_pInHandler;
    TAudioOutHandler *m_pOutHandler;

    CUSBRequest *m_pInURB[2];    // slot 0 always; slot 1 also in flight when UAC2 (no iso re-arm gap)
    CUSBRequest *m_pOutURB[2];   // double-buffered: 2 OUT URBs always in flight (no iso re-arm gap)
    CUSBRequest *m_pFbURB;

    // Bytes submitted per IN slot (pktSize*nPkts from StartInRequest). InCompletion
    // uses THIS, not pURB->GetResultLength(), to size the parse: the vendored
    // Circle DWHCI driver's GetResultLength() clamps to the LAST iso packet's
    // declared size on a multi-packet URB (dwhcixferstagedata.cpp GetResultLen
    // compares the accumulated total against a per-packet m_nTransferSize that
    // never holds the running sum) -- reliable only for single-packet (UAC1)
    // URBs. See looper-method mutable multipacket-urb-resultlength-semantics.
    unsigned m_nInSubmitBytes[2];

    // ROOT-CAUSE FIX (live-hardware-confirmed, RAWD captures + buzz.wav
    // analysis): m_nInSubmitBytes[slot] alone told InCompletion HOW MANY bytes
    // to trust in aggregate, but not WHERE they actually live in m_InBuf.
    // StartInRequest's AddIsoPacket(pktSize) is called m_nInPktsSubmitted[slot]
    // times with the SAME fixed pktSize -- the URB's actual per-microframe data
    // lands at FIXED pktSize-aligned offsets (k*pktSize), one per microframe,
    // regardless of how many bytes that microframe's real delivery was. A real
    // microframe commonly delivers FEWER bytes than pktSize (pktSize is the
    // protocol MAXIMUM; nominal rate is far below it) -- InCompletion's old
    // decode loop read m_InBuf CONTIGUOUSLY (pb + i*frameBytes) as if all
    // claimed samples were back-to-back with zero gaps, so from microframe 2
    // onward it silently walked past real data into the unused/stale tail of
    // each pktSize slot. m_nInPktSize[slot]/m_nInPktsSubmitted[slot] let
    // InCompletion re-derive each microframe's byte offset (k*pktSize)
    // correctly; m_nInSamplesPerPkt[slot][8] records exactly how many nominal
    // samples StartInRequest claimed for EACH microframe (not just the sum),
    // so InCompletion can read precisely that many samples from each slot's
    // start, skip to the next microframe's true offset, and never spill across
    // a microframe boundary into padding.
    u16      m_nInPktSize[2];
    unsigned m_nInPktsSubmitted[2];
    unsigned m_nInSamplesPerPkt[2][8];

    // UAC2 async OUT pacing. m_fbRate = frames-per-(micro)frame in Q16.16, from the
    // feedback endpoint (default = nominal 48000 / service rate); m_fbAccum carries
    // the fractional remainder so the long-run OUT rate matches the device exactly.
    u32 m_fbRate;
    u32 m_fbAccum;

    // UAC2 IN nominal-rate accumulator (Q16.16), same pattern as m_fbAccum: a
    // single truncated per-call samples/microframe estimate is a SYSTEMATIC bias
    // (e.g. 6 vs true 5.5125 at 44100Hz -- an 8.8% constant over-claim), which
    // accumulates in the IN ring's avail level fast enough to overwhelm
    // AudioSystem.cpp's drain hysteresis deadband and reproduce the periodic
    // ~1s oscillate-and-resync cycle that deadband was built to prevent
    // (audible as periodic crunch+blips). Carrying the fractional remainder
    // forward across StartInRequest calls makes the LONG-RUN average exact
    // (zero steady-state bias), matching m_fbAccum's proven approach.
    u32 m_inNomAccum;

    DMA_BUFFER (u8, m_InBuf,  USB_AUDIO_INBUF_BYTES * 2);    // two slots for double-buffering
    DMA_BUFFER (u8, m_OutBuf, USB_AUDIO_OUTBUF_BYTES * 2);   // two slots for double-buffering
    DMA_BUFFER (u8, m_FbBuf,  8);

    u32 m_nPeakIn;
    u32 m_nLastMonitorTick;

    // UAC2 (USB Audio Class 2.0, e.g. Tascam US-2x2). When m_bUAC2, samples are
    // m_uSubslot bytes each (3 = 24-bit) x m_uChannels per frame, and the sample
    // rate was set via a Clock Source control request (see Configure).
    boolean  m_bUAC2;
    unsigned m_uRate;       // negotiated sample rate (Hz)
    unsigned m_uSubslot;    // bytes per sample (UAC1 path: 2)
    unsigned m_uChannels;   // channels per frame (UAC1 path: 2)
    boolean ConfigureUAC2 (void);

    static CUSBAudioDevice *s_pThis;
    static CUSBAudioDevice *s_pOut;
    static unsigned         s_nDeviceNumber;
};

#endif
