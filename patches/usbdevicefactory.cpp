#include <circle/usb/usbdevicefactory.h>
#include <assert.h>

#include <circle/usb/usbstandardhub.h>
#include <circle/usb/usbmassdevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/usb/usbmouse.h>
#include <circle/usb/usbgamepadstandard.h>
#include <circle/usb/usbgamepadps3.h>
#include <circle/usb/usbgamepadps4.h>
#include <circle/usb/usbgamepadxbox360.h>
#include <circle/usb/usbgamepadxboxone.h>
#include <circle/usb/usbgamepadswitchpro.h>
#include <circle/usb/usbprinter.h>
#include <circle/usb/smsc951x.h>
#include <circle/usb/lan7800.h>
#include <circle/usb/usbbluetooth.h>
#include <circle/usb/usbmidi.h>
#include <circle/usb/usbcdcethernet.h>
#include "usbaudiodevice.h"

CUSBFunction *CUSBDeviceFactory::GetDevice (CUSBFunction *pParent, CString *pName)
{
	assert (pParent != 0);
	assert (pName != 0);

	CUSBFunction *pResult = 0;

	if (   pName->Compare ("int9-0-0") == 0
	    || pName->Compare ("int9-0-2") == 0)
		pResult = new CUSBStandardHub (pParent);
	else if (pName->Compare ("int8-6-50") == 0)
		pResult = new CUSBBulkOnlyMassStorageDevice (pParent);
	else if (pName->Compare ("int3-1-1") == 0)
		pResult = new CUSBKeyboardDevice (pParent);
	else if (pName->Compare ("int3-1-2") == 0)
		pResult = new CUSBMouseDevice (pParent);
	else if (pName->Compare ("int3-0-0") == 0)
		pResult = new CUSBGamePadStandardDevice (pParent);
	else if (pName->Compare ("ven54c-268") == 0)
		pResult = new CUSBGamePadPS3Device (pParent);
	else if (   pName->Compare ("ven54c-5c4") == 0
		 || pName->Compare ("ven54c-9cc") == 0)
		pResult = new CUSBGamePadPS4Device (pParent);
	else if (   pName->Compare ("ven45e-28e") == 0
		 || pName->Compare ("ven45e-28f") == 0)
		pResult = new CUSBGamePadXbox360Device (pParent);
	else if (   pName->Compare ("ven45e-2d1") == 0
		 || pName->Compare ("ven45e-2dd") == 0
		 || pName->Compare ("ven45e-2e3") == 0
		 || pName->Compare ("ven45e-2ea") == 0)
		pResult = new CUSBGamePadXboxOneDevice (pParent);
	else if (pName->Compare ("ven57e-2009") == 0)
		pResult = new CUSBGamePadSwitchProDevice (pParent);
	else if (   pName->Compare ("int7-1-1") == 0
		 || pName->Compare ("int7-1-2") == 0)
		pResult = new CUSBPrinterDevice (pParent);
	else if (pName->Compare ("ven424-ec00") == 0)
		pResult = new CSMSC951xDevice (pParent);
	else if (pName->Compare ("ven424-7800") == 0)
		pResult = new CLAN7800Device (pParent);
	else if (   pName->Compare ("inte0-1-1") == 0
		 || pName->Compare ("ven50d-65a") == 0)
		pResult = new CUSBBluetoothDevice (pParent);
	else if (   pName->Compare ("int1-3-0") == 0
		 || pName->Compare ("ven582-12a") == 0)
		pResult = new CUSBMIDIDevice (pParent);
	else if (pName->Compare ("int2-6-0") == 0)
		pResult = new CUSBCDCEthernetDevice (pParent);
	else if (pName->Compare ("int1-2-0") == 0)
		pResult = new CUSBAudioDevice (pParent);

	if (pResult != 0)
		pResult->GetDevice ()->LogWrite (LogNotice, "Using device/interface %s", (const char *) *pName);

	delete pName;
	return pResult;
}
