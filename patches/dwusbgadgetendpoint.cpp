//
// dwusbgadgetendpoint.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2023-2025  R. Stange <rsta2@gmx.net>
//
// Patched: add TypeIsochronous support (DWHCI_DEV_EP_CTRL_EP_TYPE_ISOCH)
//
#include "dwusbgadgetendpoint.h"
#include <circle/usb/gadget/dwusbgadget.h>
#include <circle/usb/dwhciregister.h>
#include <circle/logger.h>
#include <circle/debug.h>
#include <circle/util.h>
#include <assert.h>

LOGMODULE ("dwgadgetep");

#define DWHCI_DEV_STATUS			0x008
#define DWHCI_DEV_STATUS_SOFFN__SHIFT		8
#define DWHCI_DEV_STATUS_SOFFN__MASK		(0x3FFF << 8)
#define DWHCI_DEV_EP_CTRL_SET_EVEN_FRAME	DWHCI_DEV_EP_CTRL_SETDPID_D0
#define DWHCI_DEV_EP_CTRL_SET_ODD_FRAME		DWHCI_DEV_EP_CTRL_SETDPID_D1

u8 CDWUSBGadgetEndpoint::s_NextEPSeq[DWHCI_MAX_EPS_CHANNELS];
u8 CDWUSBGadgetEndpoint::s_uchFirstInNextEPSeq;

CDWUSBGadgetEndpoint::CDWUSBGadgetEndpoint (size_t nMaxPacketSize, CDWUSBGadget *pGadget)
:	m_pGadget (pGadget),
	m_Direction (DirectionInOut),
	m_Type (TypeControl),
	m_nEP (0),
	m_nMaxPacketSize (nMaxPacketSize)
{
	InitTransfer ();
	assert (m_pGadget);
	m_pGadget->AssignEndpoint (m_nEP, this);
}

CDWUSBGadgetEndpoint::CDWUSBGadgetEndpoint (const TUSBEndpointDescriptor *pDesc,
					    CDWUSBGadget *pGadget)
:	m_pGadget (pGadget),
	m_Direction (pDesc->bEndpointAddress & 0x80 ? DirectionIn : DirectionOut),
	m_Type ((pDesc->bmAttributes & 0x03) == 1 ? TypeIsochronous : TypeBulk),
	m_nEP (pDesc->bEndpointAddress & 0xF),
	m_nMaxPacketSize (pDesc->wMaxPacketSize & 0x7FF)
{
	assert ((pDesc->bmAttributes & 0x03) == 2 || (pDesc->bmAttributes & 0x03) == 1);
	InitTransfer ();
	assert (m_pGadget);
	m_pGadget->AssignEndpoint (m_nEP, this);
}

CDWUSBGadgetEndpoint::~CDWUSBGadgetEndpoint (void)
{
	assert (m_pGadget);
	m_pGadget->RemoveEndpoint (m_nEP);
}

void CDWUSBGadgetEndpoint::OnUSBReset (void)
{
	InitTransfer ();

	if (!m_nEP)
	{
		u32 nValue = 0;
		switch (m_nMaxPacketSize)
		{
		case 8:  nValue = DWHCI_DEV_EP0_CTRL_MAX_PACKET_SIZ_8;  break;
		case 16: nValue = DWHCI_DEV_EP0_CTRL_MAX_PACKET_SIZ_16; break;
		case 32: nValue = DWHCI_DEV_EP0_CTRL_MAX_PACKET_SIZ_32; break;
		case 64: nValue = DWHCI_DEV_EP0_CTRL_MAX_PACKET_SIZ_64; break;
		default: assert (0); break;
		}
		CDWHCIRegister InEP0Ctrl (DWHCI_DEV_IN_EP_CTRL (0));
		InEP0Ctrl.Read ();
		InEP0Ctrl.And (~DWHCI_DEV_EP0_CTRL_MAX_PACKET_SIZ__MASK);
		InEP0Ctrl.Or (nValue << DWHCI_DEV_EP_CTRL_MAX_PACKET_SIZ__SHIFT);
		InEP0Ctrl.Write ();
		CDWHCIRegister OutEP0Ctrl (DWHCI_DEV_OUT_EP_CTRL (0));
		OutEP0Ctrl.Read ();
		OutEP0Ctrl.And (~DWHCI_DEV_EP0_CTRL_MAX_PACKET_SIZ__MASK);
		OutEP0Ctrl.Or (nValue << DWHCI_DEV_EP_CTRL_MAX_PACKET_SIZ__SHIFT);
		OutEP0Ctrl.Write ();
	}
	else
	{
		CDWHCIRegister EPCtrl (m_Direction == DirectionIn ? DWHCI_DEV_IN_EP_CTRL (m_nEP)
								  : DWHCI_DEV_OUT_EP_CTRL (m_nEP));
		EPCtrl.Read ();
		EPCtrl.And (~DWHCI_DEV_EP_CTRL_MAX_PACKET_SIZ__MASK);
		EPCtrl.Or (m_nMaxPacketSize << DWHCI_DEV_EP_CTRL_MAX_PACKET_SIZ__SHIFT);
		EPCtrl.And (~DWHCI_DEV_EP_CTRL_EP_TYPE__MASK);
		if (m_Type == TypeIsochronous)
			EPCtrl.Or (DWHCI_DEV_EP_CTRL_EP_TYPE_ISOCH << DWHCI_DEV_EP_CTRL_EP_TYPE__SHIFT);
		else
			EPCtrl.Or (DWHCI_DEV_EP_CTRL_EP_TYPE_BULK << DWHCI_DEV_EP_CTRL_EP_TYPE__SHIFT);
		if (m_Type != TypeIsochronous)
			EPCtrl.Or (DWHCI_DEV_EP_CTRL_SETDPID_D0);
		EPCtrl.Or (DWHCI_DEV_EP_CTRL_ACTIVE_EP);
		if (m_Direction == DirectionIn)
		{
			EPCtrl.And (~DWHCI_DEV_IN_EP_CTRL_TX_FIFO_NUM__MASK);
			EPCtrl.Or (m_nEP << DWHCI_DEV_IN_EP_CTRL_TX_FIFO_NUM__SHIFT);
			unsigned i;
			for (i = 0; i <= CDWUSBGadget::NumberOfInEPs; i++)
				if (s_NextEPSeq[i] == s_uchFirstInNextEPSeq) break;
			assert (i <= CDWUSBGadget::NumberOfInEPs);
			s_NextEPSeq[i] = m_nEP;
			s_NextEPSeq[m_nEP] = s_uchFirstInNextEPSeq;
			EPCtrl.And (~DWHCI_DEV_IN_EP_CTRL_NEXT_EP__MASK);
			EPCtrl.Or (s_NextEPSeq[m_nEP] << DWHCI_DEV_IN_EP_CTRL_NEXT_EP__SHIFT);
			CDWHCIRegister DeviceConfig (DWHCI_DEV_CFG);
			DeviceConfig.Read ();
			u32 nCount = (DeviceConfig.Get () & DWHCI_DEV_CFG_EP_MISMATCH_COUNT__MASK)
				     >> DWHCI_DEV_CFG_EP_MISMATCH_COUNT__SHIFT;
			nCount++;
			DeviceConfig.And (~DWHCI_DEV_CFG_EP_MISMATCH_COUNT__MASK);
			DeviceConfig.Or (nCount << DWHCI_DEV_CFG_EP_MISMATCH_COUNT__SHIFT);
			DeviceConfig.Write ();
		}
		EPCtrl.Write ();
	}
	CDWHCIRegister AllEPsIntMask (DWHCI_DEV_ALL_EPS_INT_MASK);
	AllEPsIntMask.Read ();
	switch (m_Direction)
	{
	case DirectionInOut:
		AllEPsIntMask.Or (DWHCI_DEV_ALL_EPS_INT_MASK_IN_EP (m_nEP));
		AllEPsIntMask.Or (DWHCI_DEV_ALL_EPS_INT_MASK_OUT_EP (m_nEP));
		break;
	case DirectionOut:
		AllEPsIntMask.Or (DWHCI_DEV_ALL_EPS_INT_MASK_OUT_EP (m_nEP));
		break;
	case DirectionIn:
		AllEPsIntMask.Or (DWHCI_DEV_ALL_EPS_INT_MASK_IN_EP (m_nEP));
		break;
	}
	AllEPsIntMask.Write ();
}

void CDWUSBGadgetEndpoint::BeginTransfer (TTransferMode Mode, void *pBuffer, size_t nLength)
{
	if (!m_nEP)
		LOGWARN ("EP0 BeginTransfer mode=%u buf=%p len=%u prevmode=%u", (unsigned)Mode, pBuffer, (unsigned)nLength, (unsigned)m_TransferMode);
	assert (Mode < TransferUnknown);
	if (m_TransferMode != TransferUnknown || m_pTransferBuffer || m_nTransferLength != (size_t) -1)
	{
		LOGWARN ("EP%u BeginTransfer: auto-reset stale state (mode=%u)", m_nEP, (unsigned)m_TransferMode);
		InitTransfer ();
	}
	m_TransferMode = Mode;
	assert (pBuffer || !nLength);
	m_pTransferBuffer = pBuffer;
	m_nTransferLength = nLength;
	unsigned nPacketCount = 1;
	if (nLength)
	{
		nPacketCount = (nLength + m_nMaxPacketSize-1) / m_nMaxPacketSize;
		assert (m_nEP || nLength <= 0x7F);
		assert (m_nEP || nPacketCount <= 3);
		CleanAndInvalidateDataCacheRange ((uintptr) pBuffer, nLength);
	}
	else
		pBuffer = &m_DummyBuffer;

	if (Mode == TransferDataIn)
	{
		CDWHCIRegister InEPXferSize (DWHCI_DEV_IN_EP_XFER_SIZ (m_nEP), 0);
		InEPXferSize.Or (nPacketCount << DWHCI_DEV_EP_XFER_SIZ_PKT_CNT__SHIFT);
		InEPXferSize.Or (nLength << DWHCI_DEV_EP_XFER_SIZ_XFER_SIZ__SHIFT);
		if (m_Type == TypeIsochronous)
			InEPXferSize.Or (1 << DWHCI_DEV_EP_XFER_SIZ_MULTI_CNT__SHIFT);
		InEPXferSize.Write ();
		CDWHCIRegister InEPDMAAddress (DWHCI_DEV_IN_EP_DMA_ADDR (m_nEP),
					       BUS_ADDRESS ((uintptr) pBuffer));
		InEPDMAAddress.Write ();
		CDWHCIRegister InEPCtrl (DWHCI_DEV_IN_EP_CTRL (m_nEP), 0);
		InEPCtrl.Read ();
		InEPCtrl.And (~DWHCI_DEV_IN_EP_CTRL_NEXT_EP__MASK);
		InEPCtrl.Or (s_NextEPSeq[m_nEP] << DWHCI_DEV_IN_EP_CTRL_NEXT_EP__SHIFT);
		if (m_Type == TypeIsochronous)
		{
			CDWHCIRegister DevStatus (DWHCI_DEV_STATUS);
			u32 nFrame = (DevStatus.Read () & DWHCI_DEV_STATUS_SOFFN__MASK)
				     >> DWHCI_DEV_STATUS_SOFFN__SHIFT;
			if ((nFrame + 1) & 1)
				InEPCtrl.Or (DWHCI_DEV_EP_CTRL_SET_ODD_FRAME);
			else
				InEPCtrl.Or (DWHCI_DEV_EP_CTRL_SET_EVEN_FRAME);
		}
		InEPCtrl.Or (DWHCI_DEV_EP_CTRL_EP_ENABLE);
		InEPCtrl.Or (DWHCI_DEV_EP_CTRL_CLEAR_NAK);
		InEPCtrl.Write ();
	}
	else
	{
		CDWHCIRegister OutEPXferSize (DWHCI_DEV_OUT_EP_XFER_SIZ (m_nEP), 0);
		if (Mode == TransferSetupOut)
		{
			assert (m_nEP == 0);
			assert (nLength == sizeof (TSetupData));
			OutEPXferSize.Or (nPacketCount << DWHCI_DEV_EP0_XFER_SIZ_SETUP_PKT_CNT__SHIFT);
		}
		OutEPXferSize.Or (nPacketCount << DWHCI_DEV_EP_XFER_SIZ_PKT_CNT__SHIFT);
		OutEPXferSize.Or (nLength << DWHCI_DEV_EP_XFER_SIZ_XFER_SIZ__SHIFT);
		OutEPXferSize.Write ();
		CDWHCIRegister OutEPDMAAddress (DWHCI_DEV_OUT_EP_DMA_ADDR (m_nEP),
						BUS_ADDRESS ((uintptr) pBuffer));
		OutEPDMAAddress.Write ();
		CDWHCIRegister OutEPCtrl (DWHCI_DEV_OUT_EP_CTRL (m_nEP));
		OutEPCtrl.Read ();
		if (m_Type == TypeIsochronous)
		{
			CDWHCIRegister DevStatus (DWHCI_DEV_STATUS);
			u32 nFrame = (DevStatus.Read () & DWHCI_DEV_STATUS_SOFFN__MASK)
				     >> DWHCI_DEV_STATUS_SOFFN__SHIFT;
			if ((nFrame + 1) & 1)
				OutEPCtrl.Or (DWHCI_DEV_EP_CTRL_SET_ODD_FRAME);
			else
				OutEPCtrl.Or (DWHCI_DEV_EP_CTRL_SET_EVEN_FRAME);
		}
		OutEPCtrl.Or (DWHCI_DEV_EP_CTRL_EP_ENABLE);
		OutEPCtrl.Or (DWHCI_DEV_EP_CTRL_CLEAR_NAK);
		OutEPCtrl.Write ();
	}
}

void CDWUSBGadgetEndpoint::CancelTransfer (void)
{
	if (m_TransferMode == TransferDataIn)
	{
		CDWHCIRegister InEPCtrl (DWHCI_DEV_IN_EP_CTRL (m_nEP), 0);
		InEPCtrl.Read ();
		InEPCtrl.And (~DWHCI_DEV_EP_CTRL_EP_ENABLE);
		InEPCtrl.And (~DWHCI_DEV_EP_CTRL_CLEAR_NAK);
		InEPCtrl.Or (DWHCI_DEV_EP_CTRL_EP_DISABLE);
		InEPCtrl.Write ();
	}
	else if (m_TransferMode != TransferUnknown)
	{
		CDWHCIRegister OutEPCtrl (DWHCI_DEV_OUT_EP_CTRL (m_nEP));
		OutEPCtrl.Read ();
		OutEPCtrl.And (~DWHCI_DEV_EP_CTRL_EP_ENABLE);
		OutEPCtrl.And (~DWHCI_DEV_EP_CTRL_CLEAR_NAK);
		OutEPCtrl.Or (DWHCI_DEV_EP_CTRL_EP_DISABLE);
		OutEPCtrl.Write ();
	}
	InitTransfer ();
}

size_t CDWUSBGadgetEndpoint::FinishTransfer (void)
{
	if (m_TransferMode >= TransferUnknown)
	{
		LOGWARN ("EP%u FinishTransfer: spurious (mode=unknown)", m_nEP);
		return 0;
	}
	CDWHCIRegister EPXferSize (  m_TransferMode == TransferDataIn
				   ? DWHCI_DEV_IN_EP_XFER_SIZ (m_nEP)
				   : DWHCI_DEV_OUT_EP_XFER_SIZ (m_nEP));
	size_t nXferSize = EPXferSize.Read ();
	nXferSize &= !m_nEP ? DWHCI_DEV_EP0_XFER_SIZ_XFER_SIZ__MASK
			    : DWHCI_DEV_EP_XFER_SIZ_XFER_SIZ__MASK;
	nXferSize >>= DWHCI_DEV_EP_XFER_SIZ_XFER_SIZ__SHIFT;
	if (nXferSize > m_nTransferLength)
	{
		LOGWARN ("EP%u FinishTransfer: remaining=%u > programmed=%u mode=%u",
			 m_nEP, (unsigned)nXferSize, (unsigned)m_nTransferLength,
			 (unsigned)m_TransferMode);
		nXferSize = 0;
	}
	else
		nXferSize = m_nTransferLength - nXferSize;
	InitTransfer ();
	return nXferSize;
}

void CDWUSBGadgetEndpoint::InitTransfer (void)
{
	m_TransferMode = TransferUnknown;
	m_pTransferBuffer = nullptr;
	m_nTransferLength = -1;
}

void CDWUSBGadgetEndpoint::Stall (boolean bIn)
{
	CDWHCIRegister EPCtrl (bIn ? DWHCI_DEV_IN_EP_CTRL (m_nEP)
				   : DWHCI_DEV_OUT_EP_CTRL (m_nEP));
	EPCtrl.Read ();
	EPCtrl.Or (DWHCI_DEV_EP_CTRL_STALL);
	EPCtrl.Or (DWHCI_DEV_EP_CTRL_CLEAR_NAK);
	EPCtrl.Write ();
}

void CDWUSBGadgetEndpoint::OnControlMessage (void)
{
	LOGWARN ("EP%u base OnControlMessage called - vtable dispatch failed", m_nEP);
	assert (0);
}

void CDWUSBGadgetEndpoint::HandleOutInterrupt (void)
{
	CDWHCIRegister OutEPCommonIntMask (DWHCI_DEV_OUT_EP_COMMON_INT_MASK);
	CDWHCIRegister OutEPInt (DWHCI_DEV_OUT_EP_INT (m_nEP));
	OutEPCommonIntMask.Read ();
	OutEPInt.Read ();
	if (!m_nEP)
		LOGWARN ("EP0 OutInt raw=0x%x mask=0x%x", (unsigned)OutEPInt.Get (), (unsigned)OutEPCommonIntMask.Get ());
	OutEPInt.And (OutEPCommonIntMask.Get ());
	if (OutEPInt.Get () & DWHCI_DEV_OUT_EP_INT_SETUP_DONE)
	{
		assert (m_nEP == 0);
		CDWHCIRegister OutEPIntAck (DWHCI_DEV_OUT_EP_INT (m_nEP));
		OutEPIntAck.Set (DWHCI_DEV_OUT_EP_INT_SETUP_DONE);
		OutEPIntAck.Write ();
		{
			const u8 *p = (const u8 *) m_pTransferBuffer;
			if (p)
				LOGWARN ("EP0 SETUP data: %02x %02x %02x %02x %02x %02x %02x %02x",
					p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
			else
				LOGWARN ("EP0 SETUP_DONE: m_pTransferBuffer is NULL");
		}
		LOGWARN ("EP0 SETUP_DONE: calling OnControlMessage");
		OnControlMessage ();
		LOGWARN ("EP0 SETUP_DONE: OnControlMessage returned");
		OutEPInt.And (~DWHCI_DEV_OUT_EP_INT_XFER_COMPLETE);
	}
	if (OutEPInt.Get () & DWHCI_DEV_OUT_EP_INT_XFER_COMPLETE)
	{
		CDWHCIRegister OutEPIntAck (DWHCI_DEV_OUT_EP_INT (m_nEP));
		OutEPIntAck.Set (DWHCI_DEV_OUT_EP_INT_XFER_COMPLETE);
		OutEPIntAck.Write ();
		size_t nLength = FinishTransfer ();
		OnTransferComplete (FALSE, nLength);
	}
	if (OutEPInt.Get () & DWHCI_DEV_OUT_EP_INT_AHB_ERROR)
		LOGPANIC ("AHB error");
	if (OutEPInt.Get () & DWHCI_DEV_OUT_EP_INT_EP_DISABLED)
		LOGPANIC ("EP%u disabled", m_nEP);
}

void CDWUSBGadgetEndpoint::HandleInInterrupt (void)
{
	CDWHCIRegister InEPCommonIntMask (DWHCI_DEV_IN_EP_COMMON_INT_MASK);
	CDWHCIRegister InEPInt (DWHCI_DEV_IN_EP_INT (m_nEP));
	InEPCommonIntMask.Read ();
	InEPInt.Read ();
	if (!m_nEP)
		LOGWARN ("EP0 InInt raw=0x%x mask=0x%x", (unsigned)InEPInt.Get (), (unsigned)InEPCommonIntMask.Get ());
	InEPInt.And (InEPCommonIntMask.Get ());
	if (InEPInt.Get () & DWHCI_DEV_IN_EP_INT_XFER_COMPLETE)
	{
		CDWHCIRegister InEPIntAck (DWHCI_DEV_IN_EP_INT (m_nEP));
		InEPIntAck.Set (DWHCI_DEV_IN_EP_INT_XFER_COMPLETE);
		InEPIntAck.Write ();
		size_t nLength = FinishTransfer ();
		OnTransferComplete (TRUE, nLength);
	}
	if (InEPInt.Get () & DWHCI_DEV_IN_EP_INT_TIMEOUT)
	{
		if (m_Type == TypeIsochronous)
			LOGWARN ("Timeout iso EP%u", m_nEP);
		else
			LOGPANIC ("Timeout (EP %u)", m_nEP);
	}
	if (InEPInt.Get () & DWHCI_DEV_IN_EP_INT_AHB_ERROR)
		LOGPANIC ("AHB error");
	if (InEPInt.Get () & DWHCI_DEV_IN_EP_INT_EP_DISABLED)
		LOGPANIC ("EP%u disabled", m_nEP);
}

void CDWUSBGadgetEndpoint::HandleUSBReset (void)
{
	memset (s_NextEPSeq, 0xFF, sizeof s_NextEPSeq);
	s_NextEPSeq[0] = 0;
	s_uchFirstInNextEPSeq = 0;
}
