#
# Makefile
#

CIRCLEHOME = ../../..
PRH_HOME = ../..

OBJS = main.o kernel.o audio.o app.o apcKey25.o usbMidi.o \
       loopBuffer.o loopClip.o loopTrack.o loopMachine.o dprobe.o

LIBS = $(PRH_HOME)/audio/libaudio.a \
       $(PRH_HOME)/utils/lib_my_utils.a \
       $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

INCLUDE += -I $(PRH_HOME) -I $(PRH_HOME)/utils -I $(PRH_HOME)/audio \
           -I $(CIRCLEHOME)/addon/fatfs -I $(CIRCLEHOME)/addon/SDCard

ifdef LOOPER_USB_AUDIO
DEFINE += -DLOOPER_USB_AUDIO
endif

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
