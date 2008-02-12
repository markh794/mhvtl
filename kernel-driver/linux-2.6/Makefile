#
# $Id: Makefile,v 1.2.2.3 2006-08-30 06:35:14 markh Exp $
#
# For Debian/Ubuntu systems
# make ubuntu=1
#
# or for SuSE / RedHat systems
# make
#

CC=/usr/bin/gcc

obj-m	:= vtl.o

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
	rm -f vtl.ko

distclean:
	rm -f vtl.o vtl.ko \
	vtl.mod.o \
	vtl.mod.c \
	Modules.symvers \
	Module.symvers \
	.vtl.ko.cmd \
	.vtl.mod.o.cmd \
	.vtl.o.cmd
	rm -rf .tmp_versions
	rm -f TAGS

install:
	install -o root -g bin -m 755 vtl.ko /lib/modules/`uname -r`/kernel/drivers/scsi/

tags:
	etags *.c *.h
