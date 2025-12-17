#
# $Id: Makefile,v 1.2.2.3 2006-08-30 06:35:14 markh Exp $
#

# CC=/usr/bin/gcc
#
EXTRA_CFLAGS += -I$(SRC)/../include
EXTRA_CFLAGS += -DMHVTL_DEBUG

ccflags-y += -I$(M)/../include/common -DMHVTL_DEBUG
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

TOPDIR ?= $(CURDIR)/..
FIRMWAREDIR ?= $(shell awk '/^%.*_firmwarepath/ {print $$3}' $(TOPDIR)/mhvtl-utils.spec)

PWD	:= $(shell pwd)

all: mhvtl.ko ../mhvtl_kernel.tgz

mhvtl.ko:	config.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

config.h: config.sh
	./config.sh

install: all
	[ -d $(DESTDIR)$(FIRMWAREDIR)/mhvtl ] || mkdir -p $(DESTDIR)$(FIRMWAREDIR)/mhvtl
	install -m 755 ../mhvtl_kernel.tgz $(DESTDIR)$(FIRMWAREDIR)/mhvtl/
	install -o root -g root -m 644 mhvtl.ko /lib/modules/$(V)/kernel/drivers/scsi/
	if [ -f "/etc/modprobe.d/unsupported-modules" ]; then \
		sed -i 's/^allow_unsupported_modules 0/allow_unsupported_modules 1/' /etc/modprobe.d/unsupported-modules; \
	fi
	depmod -a

uninstall: distclean
	rm -f /lib/modules/$(V)/kernel/drivers/scsi/mhvtl.ko $(DESTDIR)$(FIRMWAREDIR)/mhvtl/mhvtl_kernel.tgz
	depmod -a

../mhvtl_kernel.tgz:  $(wildcard ./*.c) $(wildcard ./*.h) $(wildcard ../include/common/*.h) \
					  config.sh Makefile
	tar --sort=name --mtime=@1 --format=gnu --transform='s|.*/||' -czf $@ $^

install-bkup:
	cp mhvtl.ko ~/mhvtl-$(V)_`uname -m`.ko

tags:
	ctags -R *

# ========== Cleaning ==========

.PHONY:clean
clean:
	$(RM) mhvtl.ko
	$(RM) *.o
	$(RM) TAGS
	$(RM) config.h

.PHONY: distclean
distclean: clean
	$(RM) .*.o.d \
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
	    config.h
	$(RM) -r .tmp_versions