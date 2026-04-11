#include "usbaudiogadgetendpoint.h"
#include "usbaudiogadget.h"
#include <circle/util.h>
#include <assert.h>

CUSBAudioGadgetEndpoint::CUSBAudioGadgetEndpoint (const TUSBEndpointDescriptor *pDesc,
						  CUSBAudioGadget *pGadget)
:	CDWUSBGadgetEndpoint (pDesc, pGadget),
	m_pAudioGadget (pGadget),
	m_pInHandler (nullptr),
	m_pOutHandler (nullptr),
	m_nOutRingWr (0),
	m_nOutRingRd (0)
{
	memset (m_OutRingLeft,  0, sizeof m_OutRingLeft);
	memset (m_OutRingRight, 0, sizeof m_OutRingRight);
	memset (m_InBuf,  0, sizeof m_InBuf);
	memset (m_OutBuf, 0, sizeof m_OutBuf);
}

CUSBAudioGadgetEndpoint::~CUSBAudioGadgetEndpoint (void)
{
}

void CUSBAudioGadgetEndpoint::OnActivate (void)
{
	if (GetDirection () == DirectionOut)
		BeginTransfer (TransferDataOut, m_OutBuf, AUDIO_GADGET_PKT_SIZE);
	else
	{
		memset (m_InBuf, 0, AUDIO_GADGET_PKT_SIZE);
		BeginTransfer (TransferDataIn, m_InBuf, AUDIO_GADGET_PKT_SIZE);
	}
}

void CUSBAudioGadgetEndpoint::OnDeactivate (void)
{
	CancelTransfer ();
}

void CUSBAudioGadgetEndpoint::OnSuspend (void)
{
	CancelTransfer ();
}

void CUSBAudioGadgetEndpoint::OnTransferComplete (boolean bIn, size_t nLength)
{
	if (bIn)
	{
		unsigned nSamples = AUDIO_GADGET_PKT_SIZE / 4;
		s16 left[AUDIO_GADGET_PKT_SIZE / 4];
		s16 right[AUDIO_GADGET_PKT_SIZE / 4];
		if (m_pInHandler)
			(*m_pInHandler) (left, right, nSamples);
		else
		{
			memset (left,  0, nSamples * sizeof (s16));
			memset (right, 0, nSamples * sizeof (s16));
		}
		s16 *p = (s16 *) m_InBuf;
		for (unsigned i = 0; i < nSamples; i++)
		{
			p[i * 2]     = left[i];
			p[i * 2 + 1] = right[i];
		}
		BeginTransfer (TransferDataIn, m_InBuf, AUDIO_GADGET_PKT_SIZE);
	}
	else
	{
		unsigned nSamples = nLength / 4;
		if (nSamples > AUDIO_GADGET_PKT_SIZE / 4)
			nSamples = AUDIO_GADGET_PKT_SIZE / 4;
		if (nSamples > 0 && m_pOutHandler)
		{
			const s16 *p = (const s16 *) m_OutBuf;
			s16 left[AUDIO_GADGET_PKT_SIZE / 4];
			s16 right[AUDIO_GADGET_PKT_SIZE / 4];
			for (unsigned i = 0; i < nSamples; i++)
			{
				left[i]  = p[i * 2];
				right[i] = p[i * 2 + 1];
			}
			(*m_pOutHandler) (left, right, nSamples);
		}
		BeginTransfer (TransferDataOut, m_OutBuf, AUDIO_GADGET_PKT_SIZE);
	}
}
