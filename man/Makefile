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

MONTH = $(shell date -r ../ChangeLog +%B)
YEAR = $(shell date -r ../ChangeLog +%Y)

PREFIX ?= /usr
MHVTL_HOME_PATH ?= /opt/mhvtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl
MANDIR ?= /share/man

CONFIG_PATH = $(shell echo $(MHVTL_CONFIG_PATH) | sed -e s'/\//\\\//g')
HOME_PATH = $(shell echo $(MHVTL_HOME_PATH) | sed -e s'/\//\\\//g')

objects = $(patsubst %.in,%,$(wildcard *.in))

man: $(objects)

all: $(objects)

%: %.in
	sed -e s'/@VERSION@/$(VERSION)/' \
		-e s'/@MONTH@/$(MONTH)/' \
		-e s'/@YEAR@/$(YEAR)/' \
		-e s'/@CONFIG_PATH@/$(CONFIG_PATH)/' \
		-e s'/@HOME_PATH@/$(HOME_PATH)/' $< > $@

install:
	install -d -m 755 $(DESTDIR)$(PREFIX)$(MANDIR)/man1
	install -d -m 755 $(DESTDIR)$(PREFIX)$(MANDIR)/man5
	install mktape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install vtlcmd.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install vtltape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install edit_tape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install vtllibrary.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install make_vtl_media.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install dump_tape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install preload_tape.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install tapeexerciser.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install update_device.conf.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install generate_device_conf.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install generate_library_contents.1 $(DESTDIR)$(PREFIX)$(MANDIR)/man1/
	install mhvtl.conf.5 $(DESTDIR)$(PREFIX)$(MANDIR)/man5/
	install device.conf.5 $(DESTDIR)$(PREFIX)$(MANDIR)/man5/
	install library_contents.5 $(DESTDIR)$(PREFIX)$(MANDIR)/man5/

clean:
	rm -f *.1 *.5
