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

TAR_FILE := mhvtl-$(shell date +%F)-$(VERSION)$(EXTRAVERSION).tgz

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
	$(RM) -f mhvtl_kernel.tgz

.PHONY: distclean
distclean:
	$(MAKE) -C usr distclean
	$(MAKE) -C etc distclean
	$(MAKE) -C scripts distclean
	$(MAKE) -C kernel distclean
	$(MAKE) -C man clean
	$(RM) -f mhvtl_kernel.tgz
	$(RM) ../$(TAR_FILE)

install: all
	$(MAKE) -C usr install
	$(MAKE) -C scripts install
	$(MAKE) -i -C etc install
	$(MAKE) -C man man
	$(MAKE) -C man install
	[ -d $(DESTDIR)$(MHVTL_HOME_PATH) ] || mkdir -p $(DESTDIR)$(MHVTL_HOME_PATH)
	(cd kernel; tar cfz ../mhvtl_kernel.tgz *)
	[ -d $(DESTDIR)$(FIRMWAREDIR)/mhvtl ] || mkdir -p $(DESTDIR)$(FIRMWAREDIR)/mhvtl
	install -m 755 mhvtl_kernel.tgz $(DESTDIR)$(FIRMWAREDIR)/mhvtl/
ifeq ($(ROOTUID),YES)
	ldconfig
	systemctl daemon-reload
endif
	# now ensure VTL media is setup
	env LD_LIBRARY_PATH=$(DESTDIR)$(LIBDIR) \
		$(MAKE_VTL_MEDIA) \
			--config-dir=$(DESTDIR)$(MHVTL_CONFIG_PATH) \
			--home-dir=$(DESTDIR)$(MHVTL_HOME_PATH) \
			--mktape-path=usr

tar: distclean
	test -d ../$(PARENTDIR) || ln -s $(TOPDIR) ../$(PARENTDIR)
	(cd kernel; tar cfz ../mhvtl_kernel.tgz *)
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
