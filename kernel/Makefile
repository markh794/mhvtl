#
# $Id: Makefile,v 1.2.2.3 2006-08-30 06:35:14 markh Exp $
#
# For Debian/Ubuntu systems
# make ubuntu=1
#
# or for SuSE / RedHat systems
# make
#

# CC=/usr/bin/gcc
#
EXTRA_CFLAGS += -I$(SRC)/../include
EXTRA_CFLAGS += -DMHVTL_DEBUG

obj-m	:= mhvtl.o
vtl-objs := mhvtl.o

# Custom kernel build..
KDIR	:= /usr/src/linux/

ifeq ($(ubuntu),)
 KDIR	:= /lib/modules/$(shell uname -r)/build
else
 KDIR	:= /usr/src/linux-headers-$(shell uname -r)/
endif

PWD	:= $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	rm -f mhvtl.ko

distclean:
	rm -f mhvtl.o mhvtl.ko \
	*.o \
	*.o.d \
	mhvtl.mod.c \
	Modules.symvers \
	Module.symvers \
	.mhvtl.ko.cmd \
	.mhvtl.o.cmd \
	.mhvtl.mod.o.cmd \
	.event.o.cmd \
	.event.o.d \
	Module.markers \
	modules.order \
	TAGS
	rm -rf .tmp_versions

install:
	install -o root -g root -m 644 mhvtl.ko /lib/modules/`uname -r`/kernel/drivers/scsi/; \
	depmod -ae; \
	if [ -f "/etc/modprobe.d/unsupported-modules" ]; then \
		sed -e 's/^allow_unsupported_modules 0/allow_unsupported_modules 1/' /etc/modprobe.d/unsupported-modules > /etc/new; \
		/bin/mv /etc/new /etc/modprobe.d/unsupported-modules; \
	fi

install-bkup:
	cp mhvtl.ko ~/mhvtl-`uname -r`_`uname -m`.ko

tags:
	etags -R *
