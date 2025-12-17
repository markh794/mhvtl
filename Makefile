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

include config.mk

PARENTDIR = mhvtl-$(VER)
CHECK_CC = cgcc
CHECK_CC_FLAGS = '$(CHECK_CC) -Wbitwise -Wno-return-void -no-compile $(ARCH)'

TAR_FILE := mhvtl-$(shell date +%F)-$(VERSION).$(EXTRAVERSION).tgz

MAKE_VTL_MEDIA = usr/make_vtl_media

export PREFIX DESTDIR TOPDIR

CFLAGS=-Wall -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS)
CLFLAGS=-shared

all:	usr etc scripts

scripts:	patch
	$(MAKE) -C scripts

etc:	patch
	$(MAKE) -C etc

usr:	patch
	$(MAKE) -C usr

kernel: patch
	$(MAKE) -C kernel

.PHONY:check
check:	ARCH=$(shell sh scripts/checkarch.sh)
check:
	CC=$(CHECK_CC_FLAGS) $(MAKE) all


patch:


install: all
	$(MAKE) -C usr install
	$(MAKE) -C scripts install
	$(MAKE) -i -C etc install
	$(MAKE) -C man install
	$(MAKE) -C kernel install
ifeq ($(ROOTUID),YES)
	systemctl daemon-reload
endif
	# now ensure VTL media is setup
	env LD_LIBRARY_PATH=$(DESTDIR)$(LIBDIR) \
		$(MAKE_VTL_MEDIA) \
			--config-dir=$(DESTDIR)$(MHVTL_CONFIG_PATH) \
			--home-dir=$(DESTDIR)$(MHVTL_HOME_PATH) \
			--mktape-path=usr/bin

.PHONY:uninstall
uninstall:
	$(MAKE) -C usr uninstall
	$(MAKE) -C scripts uninstall
	$(MAKE) -i -C etc uninstall
	$(MAKE) -C man uninstall
	$(MAKE) -C kernel uninstall
ifeq ($(ROOTUID),YES)
	systemctl daemon-reload
endif

tar: distclean
	test -d ../$(PARENTDIR) || ln -s $(TOPDIR) ../$(PARENTDIR)
	(cd kernel; tar --transform='s|.*/||' \
		-cfz ../mhvtl_kernel.tgz * ../include/common/*)
	(cd ..;  tar cvzf $(TAR_FILE) --exclude='.git*' \
		 $(PARENTDIR)/man \
		 $(PARENTDIR)/doc \
		 $(PARENTDIR)/kernel \
		 $(PARENTDIR)/usr \
		 $(PARENTDIR)/etc/ \
		 $(PARENTDIR)/scripts/ \
		 $(PARENTDIR)/ccan/ \
		 $(PARENTDIR)/tcopy/ \
		 $(PARENTDIR)/include \
		 $(PARENTDIR)/Makefile \
		 $(PARENTDIR)/config.mk \
		 $(PARENTDIR)/README \
		 $(PARENTDIR)/INSTALL \
		 $(PARENTDIR)/ChangeLog \
		 $(PARENTDIR)/mhvtl_kernel.tgz \
		 $(PARENTDIR)/mhvtl-utils.spec)
	$(RM) ../$(PARENTDIR)

.PHONY:tags
tags:
	$(MAKE) -C usr tags
	$(MAKE) -C kernel tags

# ========== Cleaning ==========

.PHONY:clean
clean:
	$(MAKE) -C usr clean
	$(MAKE) -C etc clean
	$(MAKE) -C scripts clean
	$(MAKE) -C man clean
	$(MAKE) -C kernel clean
	$(RM) mhvtl_kernel.tgz

.PHONY: distclean
distclean:
	$(MAKE) -C usr distclean
	$(MAKE) -C etc distclean
	$(MAKE) -C scripts distclean
	$(MAKE) -C kernel distclean
	$(MAKE) -C man distclean
	$(RM) mhvtl_kernel.tgz
	$(RM) ../$(TAR_FILE)