obj-m += lgmagic.o

lgmagic-objs := lgmagic_main.o lgmagic_airmouse.o

KDIR ?= /lib/modules/$(KVER)/build
KVER ?= $(shell uname -r)
PWD  := $(shell pwd)

CFLAGS_lgmagic_airmouse.o += $(CC_FLAGS_FPU)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
