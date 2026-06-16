CIRCLEHOME = ../../..
PRH_HOME = ../..

OBJS = main.o kernel.o kernel_run.o multicore.o coreDispatch.o coreBusy.o paramSnapshot.o audio.o app.o apcKey25.o apcKey25Transpose.o apcKey25Notes.o apcKey25Filters.o usbMidi.o \
       loopBuffer.o loopClip.o loopClipUpdate.o loopClipState.o loopTrack.o loopMachine.o dprobe.o \
       continuousBuffer.o \
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

# WiFi + Ableton Link (join open net "ticker", else host "ticker" AP).
# OPT-IN (LOOPER_ENABLE_WLAN=1) — the two wedge mechanisms are now fixed in code:
#   - TX wedge: linkProcess() calls sendAlive()->SendFrame() ONLY when
#     CBcm4343Device::IsLinkUp() (abletonLink.cpp), so an un-associated radio can
#     no longer freeze Core 2; RX drain is bounded to 64 frames/tick.
#   - error-path I/O: patches/p9error.cpp no longer does blocking syslog UDP on
#     the error() longjmp (was an audio-gap / control-plane-stall source); it
#     self-heals the error-stack leak instead of asserting (host-tested:
#     scripts/test-p9-errorstack.cpp).
# Still opt-in pending HARDWARE re-validation (radio stability under sustained
# audio load is not host-provable). Build LOOPER_ENABLE_WLAN=1 to test on the Pi;
# watch :4445 WLAN (p9err= error rate, link=, wlan=joined/hosting-ticker). Flip
# the default here once a hardware run confirms a stable control plane. See
# AGENTS.md "WiFi and Ableton Link".
ifdef LOOPER_ENABLE_WLAN
DEFINE += -DLOOPER_ENABLE_WLAN
endif

include $(CIRCLEHOME)/Rules.mk

.PHONY: cstdint RubberBandStretcher.h

-include $(DEPS)
