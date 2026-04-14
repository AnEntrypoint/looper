CIRCLEHOME = ../../..
PRH_HOME = ../..

OBJS = main.o kernel.o kernel_run.o multicore.o audio.o app.o apcKey25.o apcKey25Transpose.o usbMidi.o \
       loopBuffer.o loopClip.o loopClipUpdate.o loopClipState.o loopTrack.o loopMachine.o dprobe.o \
       abletonLink.o wlanDHCP.o wlanDHCPServer.o wlan_firmware.o

LIBS = $(PRH_HOME)/audio/libaudio.a \
       $(PRH_HOME)/utils/lib_my_utils.a \
       $(CIRCLEHOME)/addon/wlan/libwlan.a \
       $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/lib/net/libnet.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

INCLUDE += -I . -I $(PRH_HOME) -I $(PRH_HOME)/utils -I $(PRH_HOME)/audio \
           -I $(CIRCLEHOME)/addon/fatfs -I $(CIRCLEHOME)/addon/SDCard \
           -I $(CIRCLEHOME)/addon/wlan \
           -I patches/signalsmith

ifdef LOOPER_USB_AUDIO
DEFINE += -DLOOPER_USB_AUDIO
endif

ifdef LOOPER_OTG_AUDIO
DEFINE += -DLOOPER_OTG_AUDIO
endif

ifdef ARM_ALLOW_MULTI_CORE
DEFINE += -DARM_ALLOW_MULTI_CORE
endif

ifdef LOOPER_LIVE_PITCH
DEFINE += -DLOOPER_LIVE_PITCH
endif

include $(CIRCLEHOME)/Rules.mk

.PHONY: cstdint RubberBandStretcher.h

-include $(DEPS)
