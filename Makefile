TARGET   = me_bench
OBJS     = main.o
CFLAGS   = -O3 -G0 -Wall -DPSP -fno-pic
ASFLAGS  = $(CFLAGS)

LDFLAGS  =
LIBS     = -lme-core -lpspgu -lpspge -lpsppower -lpspctrl -lpspusb -lpspusbstor -lm

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = ME Benchmark
PSP_EBOOT_ICON  = ICON0.PNG

BUILD_PRX = 1

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
