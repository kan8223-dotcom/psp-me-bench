# Makefile — PSP ME Benchmark
export PATH := /usr/local/pspdev/bin:$(PATH)
export PSPDEV := /usr/local/pspdev

TARGET   = me_bench
OBJS     = main.o
INCDIR   = . tiny-me
CFLAGS   = -O3 -G0 -Wall -DPSP -fno-pic
ASFLAGS  = $(CFLAGS)

LIBDIR   = tiny-me/build
LDFLAGS  =
LIBS     = -lme-core -lpspgu -lpspge -lpsppower -lpspctrl -lpspusb -lpspusbstor -lm

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = ME Benchmark
PSP_EBOOT_ICON  = ICON0.PNG

BUILD_PRX = 0
PSP_FW_VERSION = 660

PSPSDK=$(shell /usr/local/pspdev/bin/psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
