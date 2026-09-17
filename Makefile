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

# rpmbuild -ta takes the name from Source: in the spec, which is derived from
# real_version there.
TAR_FILE := mhvtl-$(REAL_VERSION).tgz

MAKE_VTL_MEDIA = usr/bin/make_vtl_media

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


# A full install covers the user-space components and the kernel module. The
# two are decoupled so the user space still installs on hosts without kernel
# build headers (containers, minimal systems) or where only user space is
# packaged: `make install-user`. The kernel step is itself graceful when no
# build tree is found (see kernel/Makefile), so plain `make install` no longer
# aborts there.
install: install-user install-kernel

.PHONY: install-user
install-user: all
	[ -d $(DESTDIR)$(MHVTL_HOME_PATH) ] || mkdir -p $(DESTDIR)$(MHVTL_HOME_PATH)
	$(MAKE) -C usr install
	$(MAKE) -C scripts install
	$(MAKE) -C etc install
	$(MAKE) -C man install
ifeq ($(ROOTUID),YES)
	@$(RELOAD_SYSTEMD)
endif

.PHONY: install-kernel
install-kernel:
	$(MAKE) -C kernel install

tape: # now ensure VTL media is setup
	env LD_LIBRARY_PATH=$(DESTDIR)$(MHVTL_LIBDIR) \
		$(MAKE_VTL_MEDIA) \
			--config-dir=$(DESTDIR)$(MHVTL_CONFIG_PATH) \
			--home-dir=$(DESTDIR)$(MHVTL_HOME_PATH) \
			--mktape-path=usr/bin

.PHONY:uninstall
uninstall:
	$(MAKE) -C usr uninstall
	$(MAKE) -C scripts uninstall
	$(MAKE) -C etc uninstall
	$(MAKE) -C man uninstall
	$(MAKE) -C kernel uninstall
ifeq ($(ROOTUID),YES)
	@$(RELOAD_SYSTEMD)
endif

tar: distclean
	test -d ../$(PARENTDIR) || ln -s $(TOPDIR) ../$(PARENTDIR)
	(cd kernel; tar --transform='s|.*/||' \
		-czf ../mhvtl_kernel.tgz * ../include/common/*)
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
