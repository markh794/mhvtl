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

# Compile the module only when KDIR is set - either auto-detected above from a
# present build tree, or passed in explicitly. Without one we still produce and
# install the source tarball (consumed by DKMS / packaging), but skip building
# and installing mhvtl.ko instead of aborting - so the module can be built
# later, e.g. by DKMS, on hosts that lack build headers now.
ifeq ($(KDIR),)
KERNEL_MODULE :=
else
KERNEL_MODULE := mhvtl.ko
endif

TOPDIR ?= $(CURDIR)/..
FIRMWAREDIR ?= $(shell awk '/^%.*_firmwarepath/ {print $$3}' $(TOPDIR)/mhvtl-utils.spec)

PWD	:= $(shell pwd)

all: $(KERNEL_MODULE) ../mhvtl_kernel.tgz
ifeq ($(KERNEL_MODULE),)
	@echo "mhvtl: no kernel build tree for $(V) found - skipping module build."
	@echo "mhvtl: install the matching kernel headers/-devel package, or pass"
	@echo "mhvtl: KDIR=<path>, to build mhvtl.ko. Source tarball staged for DKMS."
endif

mhvtl.ko:	config.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

config.h: config.sh
	./config.sh

MODULE_DIR = $(DESTDIR)/lib/modules/$(V)/kernel/drivers/scsi

# depmod describes the running system, so it is only meaningful for an install
# that lands on it, not for one staged into DESTDIR for packaging.
RUN_DEPMOD = if [ -z "$(DESTDIR)" ] && command -v depmod >/dev/null 2>&1; then depmod -a; fi

install: all
	[ -d $(DESTDIR)$(FIRMWAREDIR)/mhvtl ] || mkdir -p $(DESTDIR)$(FIRMWAREDIR)/mhvtl
	install -m 755 ../mhvtl_kernel.tgz $(DESTDIR)$(FIRMWAREDIR)/mhvtl/
ifneq ($(KERNEL_MODULE),)
	[ -d $(MODULE_DIR) ] || install -d -m 755 $(MODULE_DIR)
	install -m 644 mhvtl.ko $(MODULE_DIR)/
	$(RUN_DEPMOD)
endif

uninstall: distclean
	rm -f $(MODULE_DIR)/mhvtl.ko $(DESTDIR)$(FIRMWAREDIR)/mhvtl/mhvtl_kernel.tgz
	$(RUN_DEPMOD)

../mhvtl_kernel.tgz:  $(wildcard ./*.c) $(wildcard ./*.h) $(wildcard ../include/common/*.h) \
					  config.sh dkms.conf Makefile
	tar --sort=name --mtime=@1 --format=gnu --transform='s|.*/||' -czf $@ $^

install-bkup:
	cp mhvtl.ko ~/mhvtl-$(V)_`uname -m`.ko

tags:
	ctags -R *

# ========== Cleaning ==========

.PHONY: clean
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