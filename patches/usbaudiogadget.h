#ifndef _usbaudiogadget_h
#define _usbaudiogadget_h

#include <circle/usb/gadget/dwusbgadget.h>
#include <circle/usb/usb.h>
#include <circle/interrupt.h>
#include <circle/macros.h>
#include <circle/types.h>

class CUSBAudioGadgetEndpoint;
typedef void TAudioInHandler  (s16 *pLeft, s16 *pRight, unsigned nSamples);
typedef void TAudioOutHandler (const s16 *pLeft, const s16 *pRight, unsigned nSamples);

class CUSBAudioGadget : public CDWUSBGadget
{
public:
	CUSBAudioGadget (CInterruptSystem *pInterruptSystem,
			 u16 usVendorID  = 0x2E8A,
			 u16 usProductID = 0x000B);
	~CUSBAudioGadget (void);

	static CUSBAudioGadget *Get (void) { return s_pThis; }

	void RegisterInHandler  (TAudioInHandler  *pHandler);
	void RegisterOutHandler (TAudioOutHandler *pHandler);

protected:
	const void *GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength) override;

private:
	void AddEndpoints (void) override;
	void CreateDevice (void) override;
	void OnSuspend (void) override;

	const void *ToStringDescriptor (const char *pString, size_t *pLength);

	enum { EPOut = 1, EPIn = 2 };

	CUSBAudioGadgetEndpoint *m_pEPOut;
	CUSBAudioGadgetEndpoint *m_pEPIn;

	u8 m_StringDescBuf[80];

	static CUSBAudioGadget *s_pThis;

	struct TUSBAudioGadgetDeviceDescriptor
	{
		TUSBDeviceDescriptor Device;
	} PACKED;

	struct TUSBAudioGadgetConfigurationDescriptor
	{
		TUSBConfigurationDescriptor         Config;
		TUSBInterfaceDescriptor             AudioControl;
		u8                                  ACHeader[10];
		u8                                  IT_Out[12];
		u8                                  OT_Speaker[9];
		u8                                  IT_Mic[12];
		u8                                  OT_In[9];
		TUSBInterfaceDescriptor             StreamingOutAlt0;
		TUSBInterfaceDescriptor             StreamingOutAlt1;
		u8                                  ASOutHeader[7];
		u8                                  ASOutFormat[11];
		u8                                  ASOutEPDesc[9];
		u8                                  ASOutEPCSDesc[7];
		TUSBInterfaceDescriptor             StreamingInAlt0;
		TUSBInterfaceDescriptor             StreamingInAlt1;
		u8                                  ASInHeader[7];
		u8                                  ASInFormat[11];
		u8                                  ASInEPDesc[9];
		u8                                  ASInEPCSDesc[7];
	} PACKED;

	static const TUSBAudioGadgetDeviceDescriptor s_DeviceDescriptor;
	static const TUSBAudioGadgetConfigurationDescriptor s_ConfigDescriptor;
	static const char *const s_StringDescriptor[];
};

#endif
