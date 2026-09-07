obj-m += lgmagic.o

lgmagic-objs := lgmagic_main.o lgmagic_airmouse.o

KDIR ?= /lib/modules/$(KVER)/build
KVER ?= $(shell uname -r)
PWD  := $(shell pwd)

# lgmagic_airmouse.o is the only file with float math. Debian 12 and
# RHEL-family kernels build with -mno-sse, where any float code is a
# hard compile error ("SSE register return with SSE disabled") - opt
# this one file back into SSE. cc-option keeps the flags out on arches
# and toolchains that reject them (aarch64 still fails there with
# -mgeneral-regs-only until the fixed-point rewrite; arch/PKGBUILD
# excludes aarch64 meanwhile). lgmagic_main.c wraps every call into
# this file in kernel_fpu_begin/end, so the FPU state is always
# saved/restored the standard kernel way.
CFLAGS_lgmagic_airmouse.o += $(call cc-option,-msse -msse2)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
