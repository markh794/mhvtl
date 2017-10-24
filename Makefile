#
# This makefile needs to be invoked as follows:
#
#make <options>
#
# Here, options include:
#
# 	all 	to build all utilities
# 	clean	to clean up all intermediate files
# 	kernel	to build kernel module
#

VER = $(shell awk '/Version/ {print $$2}'  mhvtl-utils.spec)
REL = $(shell awk '/Release/ {print $$2}'  mhvtl-utils.spec | sed s/%{?dist}//g)

VERSION ?= $(VER).$(REL)
EXTRAVERSION =  $(if $(shell git show-ref 2>/dev/null),-git-$(shell git branch |awk '/\*/ {print $$2}'))

PARENTDIR = mhvtl-$(VER)
PREFIX ?= /usr
USR ?= vtl
SUSER ?=root
GROUP ?= vtl
BINGROUP ?= bin
MHVTL_HOME_PATH ?= /opt/mhvtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl
LIBDIR ?= /usr/lib
CHECK_CC = cgcc
CHECK_CC_FLAGS = '$(CHECK_CC) -Wbitwise -Wno-return-void -no-compile $(ARCH)'

export PREFIX DESTDIR

CFLAGS=-Wall -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS)
CLFLAGS=-shared

all:	usr etc scripts

scripts:	patch
	$(MAKE) -C scripts MHVTL_HOME_PATH=$(MHVTL_HOME_PATH) MHVTL_CONFIG_PATH=$(MHVTL_CONFIG_PATH)

etc:	patch
	$(MAKE) -C etc USR=$(USR) GROUP=$(GROUP) MHVTL_HOME_PATH=$(MHVTL_HOME_PATH) MHVTL_CONFIG_PATH=$(MHVTL_CONFIG_PATH)

usr:	patch
	$(MAKE) -C usr USR=$(USR) GROUP=$(GROUP) MHVTL_HOME_PATH=$(MHVTL_HOME_PATH) MHVTL_CONFIG_PATH=$(MHVTL_CONFIG_PATH)

kernel: patch
	$(MAKE) -C kernel

.PHONY:check
check:	ARCH=$(shell sh scripts/checkarch.sh)
check:
	CC=$(CHECK_CC_FLAGS) $(MAKE) all

tags:
	$(MAKE) -C usr tags
	$(MAKE) -C kernel tags

patch:

clean:
	$(MAKE) -C usr clean
	$(MAKE) -C etc clean
	$(MAKE) -C scripts clean
	$(MAKE) -C man clean

distclean:
	$(MAKE) -C usr distclean
	$(MAKE) -C etc distclean
	$(MAKE) -C scripts distclean
	$(MAKE) -C kernel distclean
	$(MAKE) -C man clean

install:
	$(MAKE) usr
	$(MAKE) -C usr install $(LIBDIR) $(PREFIX) $(DESTDIR)
	$(MAKE) scripts
	$(MAKE) -C scripts install $(PREFIX) $(DESTDIR)
	$(MAKE) etc
	$(MAKE) -i -C etc install $(DESTDIR) USR=$(USR)
	$(MAKE) -C man man
	$(MAKE) -C man install $(PREFIX) $(DESTDIR) USR=$(USR)
	test -d $(DESTDIR)/opt/mhvtl || mkdir -p $(DESTDIR)/opt/mhvtl

tar:
	$(MAKE) distclean
	test -d ../$(PARENTDIR) || ln -s mhvtl ../$(PARENTDIR)
	(cd ..;  tar cvfz /home/markh/mhvtl-`date +%F`-$(VERSION)$(EXTRAVERSION).tgz  --exclude=.git \
		 $(PARENTDIR)/man \
		 $(PARENTDIR)/doc \
		 $(PARENTDIR)/kernel \
		 $(PARENTDIR)/usr \
		 $(PARENTDIR)/etc/ \
		 $(PARENTDIR)/scripts/ \
		 $(PARENTDIR)/include \
		 $(PARENTDIR)/Makefile \
		 $(PARENTDIR)/README \
		 $(PARENTDIR)/INSTALL \
		 $(PARENTDIR)/mhvtl-utils.spec)

