//
// dwusbgadgetendpoint.h
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2023-2025  R. Stange <rsta2@gmx.net>
//
// Patched: add TypeIsochronous to TType enum
//
#ifndef _circle_usb_gadget_dwusbgadgetendpoint_h
#define _circle_usb_gadget_dwusbgadgetendpoint_h

#include <circle/usb/usb.h>
#include <circle/synchronize.h>
#include <circle/types.h>

class CDWUSBGadget;

class CDWUSBGadgetEndpoint
{
public:
	enum TDirection
	{
		DirectionOut,
		DirectionIn,
		DirectionInOut
	};

	enum TType
	{
		TypeControl,
		TypeBulk,
		TypeIsochronous
	};

	enum TTransferMode
	{
		TransferSetupOut,
		TransferDataOut,
		TransferDataIn,
		TransferUnknown
	};

public:
	CDWUSBGadgetEndpoint (size_t nMaxPacketSize, CDWUSBGadget *pGadget);
	CDWUSBGadgetEndpoint (const TUSBEndpointDescriptor *pDesc, CDWUSBGadget *pGadget);
	virtual ~CDWUSBGadgetEndpoint (void);

	virtual void OnUSBReset (void);
	virtual void OnActivate (void) = 0;
	virtual void OnDeactivate (void) = 0;
	virtual void OnTransferComplete (boolean bIn, size_t nLength) = 0;
	virtual void OnControlMessage (void);
	virtual void OnSuspend (void) {}

	TDirection GetDirection (void) const { return m_Direction; }
	TType      GetType (void) const      { return m_Type; }

protected:
	void BeginTransfer (TTransferMode Mode, void *pBuffer, size_t nLength);
	void CancelTransfer (void);
	void Stall (boolean bIn);

private:
	void InitTransfer (void);
	size_t FinishTransfer (void);

	void HandleOutInterrupt (void);
	void HandleInInterrupt (void);

	static void HandleUSBReset (void);

	friend class CDWUSBGadget;

protected:
	CDWUSBGadget *m_pGadget;
	size_t        m_nMaxPacketSize;

private:
	TDirection    m_Direction;
	TType         m_Type;
	unsigned      m_nEP;

	TTransferMode m_TransferMode;
	void         *m_pTransferBuffer;
	size_t        m_nTransferLength;
	boolean       m_bIsoOddFrame;

	DMA_BUFFER (u32, m_DummyBuffer, 1);

	static u8  s_NextEPSeq[];
	static u8  s_uchFirstInNextEPSeq;
};

#endif
