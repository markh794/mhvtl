#
# $Id: Makefile,v 1.2.2.3 2006-08-30 06:35:14 markh Exp $
#

CC=/usr/bin/gcc

obj-m	:= vtl.o

# Custom kernel build..
# KDIR	:= /usr/src/linux/

# Suitable for RedHat 4 & SLES 9
KDIR	:= /lib/modules/$(shell uname -r)/build

# For Ubuntu 6.06
#KDIR	:= /usr/src/linux-headers-$(shell uname -r)/
# KDIR	:= /usr/src/linux-headers-2.6.15-26-686/
PWD	:= $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	rm -f vtl.ko

distclean:
	rm -f vtl.o vtl.ko \
	vtl.mod.o \
	vtl.mod.c \
	.vtl.ko.cmd \
	.vtl.mod.o.cmd \
	.vtl.o.cmd
	rm -r .tmp_versions
	rm -f TAGS

install:
	install -o root -g bin -m 755 vtl.ko /lib/modules/`uname -r`/kernel/drivers/scsi/

tags:
	etags *.c *.h
