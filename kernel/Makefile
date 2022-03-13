#
# $Id: Makefile,v 1.2.2.3 2006-08-30 06:35:14 markh Exp $
#

# CC=/usr/bin/gcc
#
EXTRA_CFLAGS += -I$(SRC)/../include
EXTRA_CFLAGS += -DMHVTL_DEBUG

obj-m	:= mhvtl.o
vtl-objs := mhvtl.o

V	?= $(shell uname -r)

ifeq ($(KDIR),)
ifneq ($(wildcard /lib/modules/$(V)/build),)
KDIR := /lib/modules/$(V)/build
else
ifneq ($(wildcard /usr/src/linux-headers-$(V)),)
KDIR := /usr/src/linux-headers-$(V)
endif
endif
endif

export KDIR

PWD	:= $(shell pwd)

default:	config.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

config.h:
	./config.sh

clean:
	rm -f mhvtl.ko
	$(RM) TAGS

distclean:
	rm -f mhvtl.o mhvtl.ko \
	*.o \
	.*.o.d \
	mhvtl.mod.c \
	Modules.symvers \
	Module.symvers \
	.mhvtl.ko.cmd \
	.mhvtl.o.cmd \
	.mhvtl.mod.o.cmd \
	.event.o.cmd \
	.event.o.d \
	.Module.symvers.cmd \
	.mhvtl.mod.cmd \
	.modules.order.cmd \
	mhvtl.mod \
	Module.markers \
	modules.order \
	mhvtl.ko.unsigned \
	.mhvtl.ko.unsigned.cmd \
	TAGS \
	config.h
	rm -rf .tmp_versions

install:
	install -o root -g root -m 644 mhvtl.ko /lib/modules/$(V)/kernel/drivers/scsi/; \
	depmod -a; \
	if [ -f "/etc/modprobe.d/unsupported-modules" ]; then \
		sed -i 's/^allow_unsupported_modules 0/allow_unsupported_modules 1/' /etc/modprobe.d/unsupported-modules; \
	fi

install-bkup:
	cp mhvtl.ko ~/mhvtl-$(V)_`uname -m`.ko

tags:
	ctags -R *
