TOPDIR ?= $(CURDIR)

VER ?= $(shell awk '/Version/ {print $$2}'  $(TOPDIR)/mhvtl-utils.spec)
REL ?= $(shell awk -F'[ %]' '/Release/ {print $$2}' $(TOPDIR)/mhvtl-utils.spec)
EXTRAVERSION ?= $(shell awk '/define minor/ {print $$3}' $(TOPDIR)/mhvtl-utils.spec)

FIRMWAREDIR ?= $(shell awk '/^%.*_firmwarepath/ {print $$3}' $(TOPDIR)/mhvtl-utils.spec)

GITHASH ?= $(if $(shell test -d $(TOPDIR)/.git && echo 1),commit:\ $(shell git show -s --format=%h))
GITDATE ?= $(if $(shell test -d $(TOPDIR)/.git && echo 1),$(shell git show -s --format=%aI))

VERSION ?= $(VER)

PREFIX ?= /usr
MANDIR ?= /share/man

MHVTL_HOME_PATH ?= /opt/mhvtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl
SYSTEMD_GENERATOR_DIR ?= /usr/lib/systemd/system-generators
SYSTEMD_SERVICE_DIR ?= /usr/lib/systemd/system

# Phase 1 modernization: detect modern high-throughput compression libraries
# via pkg-config (fallback for the Makefile path; the Meson build detects these
# with dependency()). These are wired into the tape daemon's codec path in
# Phase 3; defined here so the detection/flags are ready and overridable.
LZ4_CFLAGS  ?= $(shell pkg-config --cflags liblz4 2>/dev/null)
LZ4_LIBS    ?= $(shell pkg-config --libs liblz4 2>/dev/null)
ZSTD_CFLAGS ?= $(shell pkg-config --cflags libzstd 2>/dev/null)
ZSTD_LIBS   ?= $(shell pkg-config --libs libzstd 2>/dev/null)

ifeq ($(shell whoami),root)
ROOTUID = YES
endif

# Relabelling only makes sense on a live SELinux system with the policy tools
# installed. Skip it when staging into DESTDIR for packaging - the spec file
# relabels in %post - on distributions that ship no SELinux tooling, and where
# the tools are installed but SELinux itself is disabled, in which case they
# fail and would abort the install.
SELINUX_OK = [ -z "$(DESTDIR)" ] && command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled
HAVE_RESTORECON = $(SELINUX_OK) && command -v restorecon >/dev/null 2>&1
HAVE_SEMANAGE = $(SELINUX_OK) && command -v semanage >/dev/null 2>&1

# Relabel a directory tree, when relabelling applies at all
RESTORECON_TREE = if $(HAVE_RESTORECON); then restorecon -R -v $(1); fi

# Register a systemd unit file context. A repeated install must not fail, so
# fall back to modify when the context is already defined.
define SEMANAGE_UNIT
if $(HAVE_SEMANAGE); then \
	semanage fcontext -a -t systemd_unit_file_t "$(1)" 2>/dev/null || \
	semanage fcontext -m -t systemd_unit_file_t "$(1)"; \
fi
endef

# Reloading the systemd manager only makes sense on a live systemd system.
# Skip it when staging into DESTDIR for packaging, and on non-systemd init
# systems - where systemctl is absent or systemd is not the running init - so
# that a root `make install` there does not abort (see issue #143).
RELOAD_SYSTEMD = if [ -z "$(DESTDIR)" ] && [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then systemctl daemon-reload; fi

ifeq ($(shell grep lib64$ /etc/ld.so.conf /etc/ld.so.conf.d/* | wc -l),0)
LIBDIR ?= $(PREFIX)/lib
else
LIBDIR ?= $(PREFIX)/lib64
endif

-include $(TOPDIR)/local.mk

HOME_PATH = $(subst /,\/,$(MHVTL_HOME_PATH))
CONFIG_PATH = $(subst /,\/,$(MHVTL_CONFIG_PATH))
