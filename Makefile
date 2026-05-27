CIRCLEHOME = ../../..
PRH_HOME = ../..

OBJS = main.o kernel.o kernel_run.o multicore.o coreDispatch.o coreBusy.o paramSnapshot.o audio.o app.o apcKey25.o apcKey25Transpose.o apcKey25Notes.o apcKey25Filters.o usbMidi.o \
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

# WiFi + Ableton Link (join open net "ticker", else host "ticker" AP).
# OPT-IN (LOOPER_ENABLE_WLAN=1) — NOT yet stable on hardware: with WLAN enabled
# the box boots fine (HDMI normal, ICMP answers) but Core 2's control plane
# (UDP :4444/:4445 + syslog) goes dead within a tick or two. Root cause is in the
# WiFi TX path, not RX: linkProcess() calls sendAlive() -> CBcm4343Device::
# SendFrame() unconditionally every tick, and that plan9/DWC-SDIO transmit wedges
# Core 2 when the radio isn't fully associated (no "ticker" AP present). RX-drain
# budgets (abletonLink/wlanDHCP/wlanDHCPServer) and the p9error.cpp self-heal are
# in place but insufficient alone — the SendFrame wedge must be made non-blocking
# / gated on link-up before this can default on. See AGENTS.md "WiFi" notes.
ifdef LOOPER_ENABLE_WLAN
DEFINE += -DLOOPER_ENABLE_WLAN
endif

include $(CIRCLEHOME)/Rules.mk

.PHONY: cstdint RubberBandStretcher.h

-include $(DEPS)
