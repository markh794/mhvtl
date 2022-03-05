TOPDIR ?= $(CURDIR)

VER ?= $(shell awk '/Version/ {print $$2}'  $(TOPDIR)/mhvtl-utils.spec)
REL ?= $(shell awk -F'[ %]' '/Release/ {print $$2}' $(TOPDIR)/mhvtl-utils.spec)

VERSION ?= $(VER).$(REL)
EXTRAVERSION ?= $(if $(shell git show-ref 2>/dev/null),-git-$(shell git rev-parse --abbrev-ref HEAD))

PREFIX ?= /usr
MANDIR ?= /share/man

MHVTL_HOME_PATH ?= /opt/mhvtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl
SYSTEMD_GENERATOR_DIR ?= /lib/systemd/system-generators
SYSTEMD_SERVICE_DIR ?= /lib/systemd/system

ifeq ($(shell whoami),root)
ROOTUID = YES
endif

ifeq ($(shell grep lib64$ /etc/ld.so.conf /etc/ld.so.conf.d/* | wc -l),0)
LIBDIR ?= $(PREFIX)/lib
else
LIBDIR ?= $(PREFIX)/lib64
endif

-include $(TOPDIR)/local.mk

HOME_PATH = $(subst /,\/,$(MHVTL_HOME_PATH))
CONFIG_PATH = $(subst /,\/,$(MHVTL_CONFIG_PATH))
