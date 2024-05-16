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
