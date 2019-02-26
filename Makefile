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

TOPDIR = $(shell basename $$PWD)

VERSION ?= $(VER).$(REL)
EXTRAVERSION =  $(if $(shell git show-ref 2>/dev/null),-git-$(shell git branch |awk '/\*/ {print $$2}'))

PARENTDIR = mhvtl-$(VER)
PREFIX ?= /usr
MHVTL_HOME_PATH ?= /opt/mhvtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl
CHECK_CC = cgcc
CHECK_CC_FLAGS = '$(CHECK_CC) -Wbitwise -Wno-return-void -no-compile $(ARCH)'

TAR_FILE := mhvtl-$(shell date +%F)-$(VERSION)$(EXTRAVERSION).tgz

MAKE_VTL_MEDIA = usr/make_vtl_media

export PREFIX DESTDIR

ifeq ($(shell grep lib64$ /etc/ld.so.conf /etc/ld.so.conf.d/* | wc -l),0)
LIBDIR ?= $(PREFIX)/lib
else
LIBDIR ?= $(PREFIX)/lib64
endif

CFLAGS=-Wall -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS)
CLFLAGS=-shared

all:	usr etc scripts

scripts:	patch
	$(MAKE) -C scripts MHVTL_HOME_PATH=$(MHVTL_HOME_PATH) MHVTL_CONFIG_PATH=$(MHVTL_CONFIG_PATH)

etc:	patch
	$(MAKE) -C etc MHVTL_HOME_PATH=$(MHVTL_HOME_PATH) MHVTL_CONFIG_PATH=$(MHVTL_CONFIG_PATH)

usr:	patch
	$(MAKE) -C usr MHVTL_HOME_PATH=$(MHVTL_HOME_PATH) MHVTL_CONFIG_PATH=$(MHVTL_CONFIG_PATH)

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
	$(MAKE) -C kernel clean

.PHONY: distclean
distclean:
	$(MAKE) -C usr distclean
	$(MAKE) -C etc distclean
	$(MAKE) -C scripts distclean
	$(MAKE) -C kernel distclean
	$(MAKE) -C man clean
	$(RM) ../$(TAR_FILE)

install: all
	$(MAKE) -C usr install $(LIBDIR) $(PREFIX) $(DESTDIR)
	$(MAKE) -C scripts install $(PREFIX) $(DESTDIR)
	$(MAKE) -i -C etc install $(DESTDIR)
	$(MAKE) -C man man
	$(MAKE) -C man install $(PREFIX) $(DESTDIR)
	[ -d $(DESTDIR)$(MHVTL_HOME_PATH) ] || mkdir -p $(DESTDIR)$(MHVTL_HOME_PATH)
	# now ensure VTL media is setup
	env LD_LIBRARY_PATH=$(DESTDIR)$(LIBDIR) \
		$(MAKE_VTL_MEDIA) --force \
			--config-dir=$(DESTDIR)$(MHVTL_CONFIG_PATH) \
			--home-dir=$(DESTDIR)$(MHVTL_HOME_PATH) \
			--mktape-path=usr

tar: distclean
	test -d ../$(PARENTDIR) || ln -s $(TOPDIR) ../$(PARENTDIR)
	(cd ..;  tar cvzf $(TAR_FILE) --exclude='.git*' \
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
	$(RM) ../$(PARENTDIR)
