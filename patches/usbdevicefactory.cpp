//
// usbdevicefactory.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2014-2025  R. Stange <rsta2@gmx.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <circle/usb/usbdevicefactory.h>
#include <circle/usb/usbhid.h>
#include <circle/synchronize.h>
#include <circle/sysconfig.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <assert.h>

// for factory
#include <circle/usb/usbstandardhub.h>
#include <circle/usb/usbmassdevice.h>
#include <circle/usb/usbfloppydevice.h>
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
#include <circle/usb/usbmidihost.h>
#include <circle/usb/usbaudiocontrol.h>
#include <circle/usb/usbaudiostreaming.h>
#include <circle/usb/usbcdcethernet.h>
#include <circle/usb/usbserialcdc.h>
#include <circle/usb/usbserialch341.h>
#include <circle/usb/usbserialcp210x.h>
#include <circle/usb/usbserialpl2303.h>
#include <circle/usb/usbserialft231x.h>
#include <circle/usb/usbtouchscreen.h>
#include "usbaudiodevice.h"

// Reliable cross-core audio-IN witnesses for the :4445 UAUD verb (bumped from
// CUSBAudioDevice::InCompletion). Defined here -- a plain external volatile reads
// correctly on the Core-2 control plane where the audio class statics read stale.
volatile unsigned g_audioInDeliv = 0;      // IN completions carrying audio
volatile unsigned g_audioInPeak  = 0;      // max |sample| seen on IN
volatile unsigned g_audioInSubmitFail = 0; // StartInRequest submit failures
volatile unsigned g_audioOutDeliv = 0;     // OUT completions (iso OUT transfers)
volatile unsigned g_audioOutPeak  = 0;     // max |sample| sent to OUT
volatile unsigned g_audioOutSubmitFail = 0;// StartOutRequest submit failures
volatile unsigned g_audioFbRate   = 0;     // UAC2 feedback rate (Q16.16 frames/uframe)
volatile unsigned g_audioFbCount  = 0;     // UAC2 feedback URBs completed
volatile unsigned g_audioOutMaxGapUs = 0;  // max gap between OUT completions (us, reset on UAUD read)
volatile unsigned g_audioOutLastTick = 0;  // last OUT completion tick (us)
volatile unsigned g_audioInMaxGapUs  = 0;  // max gap between IN completions (us, reset on UAUD read)

// Raw-input zero-crossing-rate accumulator (:4445 UAUD verb, izcL/izcR fields).
// Sampled at the EARLIEST point in the audio pipeline -- inside InCompletion,
// right after decoding raw USB bytes to s16, before loopMachine or any effect
// touches the signal. A continuous tonal/buzz artifact (vs clean audio or
// silence) has a distinctive, elevated zero-crossing rate; comparing this
// raw-input ZCR against downstream telemetry (e.g. the wet mix) is the
// cheapest available way to localize whether a reported "buzz on input" noise
// is genuinely present at the USB wire, or only appears after the DSP chain
// -- without needing a WAV capture or physical drive retrieval.
volatile unsigned g_audioInZcL = 0;        // raw-input zero-crossings, left channel
volatile unsigned g_audioInZcR = 0;        // raw-input zero-crossings, right channel
volatile unsigned g_audioInLastTick  = 0;  // last IN completion tick (us)

// Raw-input mean-absolute-amplitude accumulator (:4445 UAUD verb, ienL/ienR
// fields -- "input energy"). ZCR (above) proved BLIND to a real, user-
// confirmed buzz occurrence this session: live samples during the audible
// buzz showed LOWER zero-crossing-rate than idle, not higher -- ZCR only
// detects HIGH-frequency content well, so a low-frequency artifact (mains
// hum, a beat tone, broadband noise with more low-end than high-end energy)
// would pass through nearly invisible to it. This accumulator sums
// |sample| (mean-absolute-amplitude -- a cheap RMS-like energy proxy that
// avoids a multiply in the ISR-critical InCompletion path) across every
// raw-input sample, regardless of frequency, so an elevated noise FLOOR of
// any spectral shape shows up as elevated average magnitude. Reset-on-read
// like izcL/izcR; divide by sample count (also exposed) to get true mean
// amplitude per window.
volatile unsigned long long g_audioInEnergyL = 0;  // sum(|sample|), left
volatile unsigned long long g_audioInEnergyR = 0;  // sum(|sample|), right
volatile unsigned g_audioInEnergyN = 0;            // sample count this window

// Raw-input sample SNAPSHOT (:4445 RAWD verb). ZCR and RMS-energy telemetry
// (above) both proved BLIND to a real, always-on, user-confirmed buzz+
// distortion on this hardware -- every live sample this session showed flat,
// unremarkable scalar statistics, even one taken while the user explicitly
// confirmed the artifact was present at that exact moment. Scalar summary
// metrics cannot catch every failure shape (a narrow spectral feature,
// intermodulation product, or bit-level corruption can leave gross
// statistics untouched). This buffer holds real WAVEFORM data instead --
// continuously overwritten by InCompletion at the SAME raw-input tap point
// (right after USB decode, before loopMachine/effects) so a RAWD request
// always returns a genuinely recent snapshot, no separate trigger/arm step
// needed. RAWD_SNAP_SAMPLES sized so the hex dump (4 hex chars/sample x2
// channels) fits comfortably under a single UDP datagram's practical MTU.
#define RAWD_SNAP_SAMPLES 128
volatile s16 g_audioInSnapL[RAWD_SNAP_SAMPLES];
volatile s16 g_audioInSnapR[RAWD_SNAP_SAMPLES];
volatile unsigned g_audioInSnapSeq = 0;   // bumped each time the snapshot is refreshed

// Per-slot completion counters (:4445 UAUD verb, slot0/slot1 fields). The real
// buzz.wav recording shows a periodic artifact at EXACTLY 500Hz -- half of the
// UAC2 IN N=8 microframe batching's 1000 completions/sec (8000/8). If only ONE
// of the two double-buffered slots (0 or 1) is producing bad data, that would
// show as a 500Hz-periodic artifact even though total completions run at
// 1000/sec. These counters let a live probe confirm slot 0 and slot 1 are
// completing at the same rate (ruling out an asymmetry) or catch a skew.
volatile unsigned g_audioInSlot0Count = 0;
volatile unsigned g_audioInSlot1Count = 0;

CUSBFunction *CUSBDeviceFactory::GetDevice (CUSBFunction *pParent, CString *pName)
{
	assert (pParent != 0);
	assert (pName != 0);

	const char *pUSBIgnore = CKernelOptions::Get ()->GetUSBIgnore ();
	assert (pUSBIgnore != 0);
	if (pName->Compare (pUSBIgnore) == 0)
	{
		CLogger::Get ()->Write ("ufactory", LogWarning,
					"Ignoring device/interface %s", pUSBIgnore);

		return 0;
	}

	CUSBFunction *pResult = 0;

	if (   pName->Compare ("int9-0-0") == 0
	    || pName->Compare ("int9-0-2") == 0)
	{
		pResult = new CUSBStandardHub (pParent);
	}
#ifndef EXCLUDE_USB_STORAGE
	else if (pName->Compare ("int8-6-50") == 0)
	{
		pResult = new CUSBBulkOnlyMassStorageDevice (pParent);
	}
	else if (   pName->Compare ("int8-4-0") == 0
		 || pName->Compare ("int8-4-1") == 0)
	{
		pResult = new CUSBFloppyDiskDevice (pParent);
	}
#endif
#ifndef EXCLUDE_USB_KEYB
	else if (pName->Compare ("int3-1-1") == 0)
	{
		CString *pVendor = pParent->GetDevice ()->GetName (DeviceNameVendor);
		assert (pVendor != 0);

		if (pVendor->Compare ("ven3f0-1198") != 0)	// HP USB 1000dpi Laser Mouse
		{
			pResult = new CUSBKeyboardDevice (pParent);
		}

		delete pVendor;
	}
#endif
#ifndef EXCLUDE_USB_MOUSE
	else if (pName->Compare ("int3-1-2") == 0)
	{
		pResult = new CUSBMouseDevice (pParent);
	}
#endif
	else if (   pName->Compare ("int3-0-0") == 0
		 || pName->Compare ("int3-0-2") == 0
		 || pName->Compare ("int3-1-0") == 0)
	{
		CString *pVendor = pParent->GetDevice ()->GetName (DeviceNameVendor);
		assert (pVendor != 0);

		if (pVendor->Compare ("ven5ac-21e") != 0)	// Apple Aluminum Mini Keyboard
		{
			pResult = GetGenericHIDDevice (pParent);
		}

		delete pVendor;
	}
#ifndef EXCLUDE_USB_GAMEPAD
	else if (pName->Compare ("ven54c-268") == 0)
	{
		pResult = new CUSBGamePadPS3Device (pParent);
	}
	else if (   pName->Compare ("ven54c-5c4") == 0
		 || pName->Compare ("ven54c-9cc") == 0)
	{
		pResult = new CUSBGamePadPS4Device (pParent);
	}
	else if (   pName->Compare ("ven45e-28e") == 0
		 || pName->Compare ("ven45e-28f") == 0)
	{
		pResult = new CUSBGamePadXbox360Device (pParent);
	}
	else if (   pName->Compare ("ven45e-2d1") == 0		// XBox One Controller
		 || pName->Compare ("ven45e-2dd") == 0		// XBox One Controller (FW 2015)
		 || pName->Compare ("ven45e-2e3") == 0		// XBox One Elite Controller
		 || pName->Compare ("ven45e-2ea") == 0		// XBox One S Controller
		 || pName->Compare ("ven45e-b12") == 0)		// XBox Series X Controller
	{
		pResult = new CUSBGamePadXboxOneDevice (pParent);
	}
	else if (pName->Compare ("ven57e-2009") == 0)
	{
		pResult = new CUSBGamePadSwitchProDevice (pParent);
	}
#endif
#ifndef EXCLUDE_USB_PRINTER
	else if (   pName->Compare ("int7-1-1") == 0
		 || pName->Compare ("int7-1-2") == 0)
	{
		pResult = new CUSBPrinterDevice (pParent);
	}
#endif
#ifndef EXCLUDE_USB_NET
	else if (pName->Compare ("ven424-ec00") == 0)
	{
		pResult = new CSMSC951xDevice (pParent);
	}
	else if (pName->Compare ("ven424-7800") == 0)
	{
		pResult = new CLAN7800Device (pParent);
	}
#endif
#ifndef EXCLUDE_USB_BLUETOOTH
	else if (   pName->Compare ("inte0-1-1") == 0
		 || pName->Compare ("ven50d-65a") == 0)		// Belkin F8T065BF Mini Bluetooth 4.0 Adapter
	{
		pResult = new CUSBBluetoothDevice (pParent);
	}
#endif
#ifndef EXCLUDE_USB_MIDI
	else if (   pName->Compare ("int1-3-0") == 0
		 || pName->Compare ("ven582-12a") == 0		// Roland UM-ONE MIDI interface
		 || pName->Compare ("ven582-28c") == 0)		// Roland JD-08
	{
		pResult = new CUSBMIDIHostDevice (pParent);
	}
#endif
#ifndef EXCLUDE_USB_AUDIO
#if RASPPI >= 4
	else if (   pName->Compare ("int1-2-0") == 0
		 || pName->Compare ("int1-2-20") == 0)
	{
		pResult = new CUSBAudioDevice (pParent);
	}
#endif
#endif
#ifndef EXCLUDE_USB_NET
	else if (pName->Compare ("int2-6-0") == 0)
	{
		pResult = new CUSBCDCEthernetDevice (pParent);
	}
#endif
#ifndef EXCLUDE_USB_SERIAL
	else if (   pName->Compare ("int2-2-0") == 0
		 || pName->Compare ("int2-2-1") == 0)
	{
		pResult = new CUSBSerialCDCDevice (pParent);
	}
	else if (FindDeviceID (pName, CUSBSerialCH341Device::GetDeviceIDTable ()))
	{
		pResult = new CUSBSerialCH341Device (pParent);
	}
	else if (FindDeviceID (pName, CUSBSerialCP210xDevice::GetDeviceIDTable ()))
	{
		pResult = new CUSBSerialCP210xDevice (pParent);
	}
	else if (FindDeviceID (pName, CUSBSerialPL2303Device::GetDeviceIDTable ()))
	{
		pResult = new CUSBSerialPL2303Device (pParent);
	}
	else if (FindDeviceID (pName, CUSBSerialFT231XDevice::GetDeviceIDTable ()))
	{
		pResult = new CUSBSerialFT231XDevice (pParent);
	}
#endif
	// new devices follow

	if (pResult != 0)
	{
		pResult->GetDevice ()->LogWrite (LogNotice, "Using device/interface %s", (const char *) *pName);
	}

	delete pName;

	return pResult;
}

CUSBFunction *CUSBDeviceFactory::GetGenericHIDDevice (CUSBFunction *pParent)
{
#ifndef EXCLUDE_USB_TOUCHSCREEN
	CUSBFunction TempFunction (pParent);

	TUSBHIDDescriptor *pHIDDesc =
		(TUSBHIDDescriptor *) TempFunction.GetDescriptor (DESCRIPTOR_HID);
	if (   pHIDDesc == 0
	    || pHIDDesc->wReportDescriptorLength == 0)
	{
		TempFunction.ConfigurationError ("usbhid");

		return 0;
	}

	u16 usReportDescriptorLength = pHIDDesc->wReportDescriptorLength;
	DMA_BUFFER (u8, ReportDescriptor, usReportDescriptorLength);

	if (   TempFunction.GetHost ()->GetDescriptor (
			TempFunction.GetEndpoint0 (),
			pHIDDesc->bReportDescriptorType, DESCRIPTOR_INDEX_DEFAULT,
			ReportDescriptor, usReportDescriptorLength,
			REQUEST_IN | REQUEST_TO_INTERFACE, TempFunction.GetInterfaceNumber ())
	    != usReportDescriptorLength)
	{
		TempFunction.GetDevice ()->LogWrite (LogError, "Cannot get HID report descriptor");

		return 0;
	}

	const u8 *pDesc = ReportDescriptor;
	for (u16 nDescSize = usReportDescriptorLength; nDescSize;)
	{
		u8 ucItem = *pDesc++;
		nDescSize--;

		u32 nArg;

		switch (ucItem & 0x03)
		{
		case 0:
			nArg = 0;
			break;

		case 1:
			nArg = *pDesc++;
			nDescSize--;
			break;

		case 2:
			nArg  = *pDesc++;
			nArg |= *pDesc++ << 8;
			nDescSize -= 2;
			break;

		case 3:
			nArg  = *pDesc++;
			nArg |= *pDesc++ << 8;
			nArg |= *pDesc++ << 16;
			nArg |= *pDesc++ << 24;
			nDescSize -= 4;
			break;
		}

		ucItem &= 0xFC;

		if (   ucItem == 0x04		// Usage Page (Digitizer)
		    && nArg == 0x0D)
		{
			return new CUSBTouchScreenDevice (pParent);
		}
	}
#endif

#ifndef EXCLUDE_USB_GAMEPAD
	return new CUSBGamePadStandardDevice (pParent);
#else
	return 0;
#endif
}

boolean CUSBDeviceFactory::FindDeviceID (CString *pName, const TUSBDeviceID *pIDTable)
{
	while (   pIDTable->usVendorID != 0
	       || pIDTable->usDeviceID != 0)
	{
		CString String;
		String.Format ("ven%x-%x", (unsigned) pIDTable->usVendorID,
					   (unsigned) pIDTable->usDeviceID);

		if (pName->Compare (String) == 0)
		{
			return TRUE;
		}

		pIDTable++;
	}

	return FALSE;
}
