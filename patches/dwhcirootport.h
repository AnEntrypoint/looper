#ifndef _circle_usb_dwhcirootport_h
#define _circle_usb_dwhcirootport_h

#include <circle/usb/usbhcirootport.h>
#include <circle/usb/usbdevice.h>
#include <circle/types.h>

class CDWHCIDevice;

class CDWHCIRootPort : public CUSBHCIRootPort
{
public:
	CDWHCIRootPort (CDWHCIDevice *pHost);
	~CDWHCIRootPort (void);

	boolean Initialize (void);

	boolean ReScanDevices (void);
	boolean RemoveDevice (void);

#if RASPPI >= 4
	u8 GetPortID (void) const { return 0; }
#endif

private:
	CDWHCIDevice *m_pHost;

	CUSBDevice *m_pDevice;
};

#endif
