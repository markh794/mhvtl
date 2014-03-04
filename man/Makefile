#
# This makefile needs to be invoked as follows:
#
#make <options>
#
# Here, options include:
#
# 	all 	to build all utilities
# 	clean	to clean up all intermediate files
#
#
# Makefile magic
# $@ is a variable that expands to the name of the file being built
# $< is a variable that expands to the naem of the source file
# @ at the beginning of the first line tell make not to echo the commands as it run it.
#

VER = $(shell awk '/Version/ {print $$2}' ../mhvtl-utils.spec)
REL = $(shell awk '/Release/ {print $$2}' ../mhvtl-utils.spec)

VERSION ?= $(VER)
EXTRAVERSION =  $(if $(shell git show-ref 2>/dev/null),-git-$(shell git show-ref --head --abbrev|head -1|awk '{print $$1}'))

ifeq ($(shell uname -m),x86_64)
LIBDIR=lib64
else
LIBDIR=lib
endif

# Ubuntu / mint seem to have stopped using /lib64/
ifeq ($(shell grep lib64 /etc/ld.so.conf.d/* | wc -l),0)
LIBDIR=lib
endif

MONTH = $(shell date +%B)
YEAR = $(shell date +%Y)

PREFIX ?= /usr
USR ?= vtl
SUSER = root
GROUP ?= vtl
MHVTL_HOME_PATH ?= /opt/mhvtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl
MANDIR ?= /share/man

CFLAGS=-Wall -Wshadow -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS) -I../kernel
CFLAGS += -DMHVTL_VERSION=\"$(VERSION)$(EXTRAVERSION)\"
CFLAGS += -DMHVTL_DEBUG
CFLAGS += -DUSR=\"$(USR)\"
CFLAGS += -DMHVTL_HOME_PATH=\"$(MHVTL_HOME_PATH)\"
CFLAGS += -DMHVTL_CONFIG_PATH=\"$(MHVTL_CONFIG_PATH)\"

CLFLAGS=-shared ${RPM_OPT_FLAGS}

objects = $(patsubst %.in,%,$(wildcard *.in))

all: $(objects)

$(objects) :
	sed -e s'/@VERSION@/$(VERSION)/' $@.in > $@
	@sed -i s'/@MONTH@/$(MONTH)/' $@
	@sed -i s'/@YEAR@/$(YEAR)/' $@

install:
	install -d -m 755 $(DESTDIR)$(PREFIX)$(MANDIR)/man1
	install -d -m 755 $(DESTDIR)$(PREFIX)$(MANDIR)/man5
	install -o $(USER) mhvtl.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install -o $(USER) mktape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install -o $(USER) vtlcmd.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install -o $(USER) vtltape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install -o $(USER) edit_tape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install -o $(USER) vtllibrary.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install -o $(USER) make_vtl_media.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install -o $(USER) build_library_config.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install -o $(USER) mhvtl.conf.5 $(DESTDIR)$(PREFIX)$(MANDIR)/man5/
	install -o $(USER) device.conf.5 $(DESTDIR)$(PREFIX)$(MANDIR)/man5/
	install -o $(USER) library_contents.5 $(DESTDIR)$(PREFIX)$(MANDIR)/man5/

clean:
	rm *.1 *.5
