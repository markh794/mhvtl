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

MONTH = $(shell date -r ../ChangeLog +%B)
YEAR = $(shell date -r ../ChangeLog +%Y)

PREFIX ?= /usr
MHVTL_HOME_PATH ?= /opt/mhvtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl
MANDIR ?= /share/man

objects = $(patsubst %.in,%,$(wildcard *.in))

man: $(objects)

all: $(objects)

$(objects) :
	sed -e s'/@VERSION@/$(VERSION)/' $@.in > $@
	@sed -i s'/@MONTH@/$(MONTH)/' $@
	@sed -i s'/@YEAR@/$(YEAR)/' $@

install:
	install -d -m 755 $(DESTDIR)$(PREFIX)$(MANDIR)/man1
	install -d -m 755 $(DESTDIR)$(PREFIX)$(MANDIR)/man5
	install mhvtl.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install mktape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install vtlcmd.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install vtltape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install edit_tape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install vtllibrary.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install make_vtl_media.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install build_library_config.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install dump_tape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install tapeexerciser.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install update_device.conf.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install mhvtl.conf.5 $(DESTDIR)$(PREFIX)$(MANDIR)/man5/
	install device.conf.5 $(DESTDIR)$(PREFIX)$(MANDIR)/man5/
	install library_contents.5 $(DESTDIR)$(PREFIX)$(MANDIR)/man5/

clean:
	rm -f *.1 *.5
