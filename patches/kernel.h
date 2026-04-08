#ifndef _kernel_h
#define _kernel_h

#include <circle/memory.h>
#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/serial.h>
#include <circle/screen.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/gadget/usbcdcgadget.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/syslogdaemon.h>
#include <circle/net/ipaddress.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <wlan/bcm4343.h>
#include <circle/types.h>

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

#define NET_OWN_IP		192, 168, 137, 100
#define NET_NETMASK		255, 255, 255, 0
#define NET_GATEWAY		192, 168, 137, 1
#define NET_DNS			192, 168, 137, 1
#define NET_LOG_HOST		192, 168, 137, 1

class CKernel
{
public:
	CKernel(void);
	~CKernel(void);

	boolean Initialize(void);
	TShutdownMode Run(void);

private:
	CMemorySystem		m_Memory;
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CSerialDevice		m_Serial;
	CScreenDevice		m_Screen;
	CLogger			m_Logger;
	CScheduler		m_Scheduler;
	CUSBHCIDevice		m_USBHCI;
	CUSBCDCGadget		m_CDCGadget;
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;
	CBcm4343Device		m_WLAN;
	CNetSubSystem		m_Net;
	CSysLogDaemon		*m_pSysLog;
};

#endif
