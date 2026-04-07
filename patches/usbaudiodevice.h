#ifndef _usbaudiodevice_h
#define _usbaudiodevice_h

#include <circle/usb/usbfunction.h>
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbrequest.h>
#include <circle/types.h>

#define USB_AUDIO_BLOCK_BYTES   256
#define USB_AUDIO_NUM_BUFS      4

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

private:
    boolean StartInRequest  (void);
    boolean StartOutRequest (void);

    void InCompletion  (CUSBRequest *pURB);
    void OutCompletion (CUSBRequest *pURB);

    static void InStub  (CUSBRequest *pURB, void *pParam, void *pContext);
    static void OutStub (CUSBRequest *pURB, void *pParam, void *pContext);

    CUSBEndpoint *m_pEndpointIn;
    CUSBEndpoint *m_pEndpointOut;

    TAudioInHandler  *m_pInHandler;
    TAudioOutHandler *m_pOutHandler;

    CUSBRequest *m_pInURB;
    CUSBRequest *m_pOutURB;

    u8 m_InBuf [USB_AUDIO_BLOCK_BYTES];
    u8 m_OutBuf[USB_AUDIO_BLOCK_BYTES];

    u32 m_nPeakIn;
    u32 m_nLastMonitorTick;

    static CUSBAudioDevice *s_pThis;
    static unsigned         s_nDeviceNumber;
};

#endif
