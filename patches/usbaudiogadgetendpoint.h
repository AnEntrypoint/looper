#ifndef _usbaudiogadgetendpoint_h
#define _usbaudiogadgetendpoint_h

#include <circle/usb/gadget/dwusbgadgetendpoint.h>
#include <circle/types.h>

class CUSBAudioGadget;

typedef void TAudioInHandler  (s16 *pLeft, s16 *pRight, unsigned nSamples);
typedef void TAudioOutHandler (const s16 *pLeft, const s16 *pRight, unsigned nSamples);

#define AUDIO_GADGET_RING_SIZE  256
#define AUDIO_GADGET_PKT_SIZE   192

class CUSBAudioGadgetEndpoint : public CDWUSBGadgetEndpoint
{
public:
	CUSBAudioGadgetEndpoint (const TUSBEndpointDescriptor *pDesc, CUSBAudioGadget *pGadget);
	~CUSBAudioGadgetEndpoint (void);

	void RegisterInHandler  (TAudioInHandler  *pHandler) { m_pInHandler  = pHandler; }
	void RegisterOutHandler (TAudioOutHandler *pHandler) { m_pOutHandler = pHandler; }

	void OnActivate   (void) override;
	void OnDeactivate (void) override;
	void OnSuspend    (void) override;
	void OnTransferComplete (boolean bIn, size_t nLength) override;

private:
	CUSBAudioGadget  *m_pAudioGadget;
	TAudioInHandler  *m_pInHandler;
	TAudioOutHandler *m_pOutHandler;

	DMA_BUFFER (u8, m_InBuf,  AUDIO_GADGET_PKT_SIZE);
	DMA_BUFFER (u8, m_OutBuf, AUDIO_GADGET_PKT_SIZE);

	s16 m_OutRingLeft  [AUDIO_GADGET_RING_SIZE];
	s16 m_OutRingRight [AUDIO_GADGET_RING_SIZE];
	volatile unsigned m_nOutRingWr;
	volatile unsigned m_nOutRingRd;
};

#endif
