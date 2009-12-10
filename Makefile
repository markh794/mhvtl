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

VER = $(shell grep Version mhvtl.spec|awk '{print $$2}')
REL = $(shell grep Release mhvtl.spec|awk '{print $$2}')

VERSION ?= $(VER).$(REL)
EXTRAVERSION =  $(if $(shell git show-ref 2>/dev/null),-git-$(shell git show-ref --head --abbrev|head -1|awk '{print $$1}'))

PARENTDIR = mhvtl-$(VER)
PREFIX ?= /usr
USR = vtl
SUSER ?=root
GROUP = vtl
MHVTL_HOME_PATH ?= /opt/mhvtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl

export PREFIX DESTDIR

CFLAGS=-Wall -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS)
CLFLAGS=-shared

all:	usr etc

etc:	patch
	$(MAKE) -C etc USR=$(USR) GROUP=$(GROUP) MHVTL_HOME_PATH=$(MHVTL_HOME_PATH) MHVTL_CONFIG_PATH=$(MHVTL_CONFIG_PATH)

usr:	patch
	$(MAKE) -C usr USR=$(USR) GROUP=$(GROUP) MHVTL_HOME_PATH=$(MHVTL_HOME_PATH) MHVTL_CONFIG_PATH=$(MHVTL_CONFIG_PATH)

kernel: patch
	$(MAKE) -C kernel

tags:
	$(MAKE) -C usr tags
	$(MAKE) -C kernel tags

patch:

clean:
	$(MAKE) -C usr clean
	$(MAKE) -C etc clean

distclean:
	$(MAKE) -C usr distclean
	$(MAKE) -C etc distclean
	$(MAKE) -C kernel distclean

install:
	$(MAKE) usr
	$(MAKE) -C usr install $(PREFIX) $(DESTDIR)
	$(MAKE) etc
	$(MAKE) -C etc install

tar:
	$(MAKE) distclean
	$(MAKE) etc
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
		 $(PARENTDIR)/mhvtl.spec)

