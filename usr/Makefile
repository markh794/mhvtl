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

VER = $(shell grep Version ../mhvtl.spec|awk '{print $$2}')
REL = $(shell grep Release ../mhvtl.spec|awk '{print $$2}')

VERSION ?= $(VER).$(REL)
EXTRAVERSION =  $(if $(shell git show-ref 2>/dev/null),-git-$(shell git show-ref --head --abbrev|head -1|awk '{print $$1}'))

ifeq ($(shell uname -m),x86_64)
LIBDIR=lib64
else
LIBDIR=lib
endif

PREFIX ?= /usr
USR = vtl
SUSER = root
GROUP = vtl
MHVTL_HOME_PATH ?= /opt/vtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl
CONFIG_PATH = $(shell echo $(MHVTL_CONFIG_PATH) | sed -e s'/\//\\\//g')
HOME_PATH = $(shell echo $(MHVTL_HOME_PATH) | sed -e s'/\//\\\//g')

CFLAGS=-Wall -Wshadow -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS) -I../kernel
CFLAGS += -DMHVTL_VERSION=\"$(VERSION)$(EXTRAVERSION)\"
CFLAGS += -DMHVTL_DEBUG
CFLAGS += -DUSR=\"$(USR)\"
CFLAGS += -DMHVTL_HOME_PATH=\"$(MHVTL_HOME_PATH)\"
CFLAGS += -DMHVTL_CONFIG_PATH=\"$(MHVTL_CONFIG_PATH)\"

CLFLAGS=-shared ${RPM_OPT_FLAGS}

ifeq ($(shell uname -m),x86_64)
LIBDIR = lib64
else
LIBDIR = lib
endif

all:	libvtlscsi.so vtltape dump_tape vtlcmd dump_messageQ mktape \
	vtllibrary make_vtl_media

libvtlscsi.so:	vtllib.c spc.c vtllib.h scsi.h \
		be_byteshift.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -c -fpic vtllib.c
	$(CC) $(CFLAGS) -c -fpic spc.c
	$(CC) $(CLFLAGS) -o libvtlscsi.so vtllib.o spc.o

dump_messageQ:	dump_messageQ.o q.o q.h
	$(CC) $(CFLAGS) -o dump_messageQ dump_messageQ.o q.o

vtlcmd:	vtlcmd.o q.o q.h
	$(CC) $(CFLAGS) -o vtlcmd q.o vtlcmd.o

dump_tape:	dump_tape.o vtlcart.o libvtlscsi.so vtltape.h scsi.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o dump_tape dump_tape.o vtlcart.o -L. -lvtlscsi

mktape:		mktape.o vtlcart.o libvtlscsi.so vtltape.h q.h vtllib.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o mktape mktape.o vtlcart.o -L. -lvtlscsi

vtllibrary:	vtllibrary.o q.o q.h vtllib.h scsi.h \
		libvtlscsi.so \
		be_byteshift.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o vtllibrary vtllibrary.o q.o -L. -lvtlscsi

vtltape:	vtltape.o vtlcart.o q.o q.h vtllib.h vtltape.h scsi.h \
		libvtlscsi.so \
		be_byteshift.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o vtltape vtltape.o vtlcart.o q.o -lz -L. -lvtlscsi

make_vtl_media:	make_vtl_media.in
	sed -e s'/@HOME_PATH@/$(HOME_PATH)/' $< > $@.1
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' $@.1 > $@
	rm -f $@.1

clean:
	rm -f vtltape.o dump_tape.o q.o \
		vtlcmd.o q.o dump_messageQ.o core mktape.o \
		vtllib.o libvtlscsi.so z.o vtllibrary.o \
		vtlcart.o spc.o

tags:
	etags *.c *.h ../kernel/*.h

distclean:
	rm -f vtltape.o vtltape \
	dump_tape.o dump_tape \
	q.o q \
	vtlcmd.o vtlcmd \
	dump_messageQ.o dump_messageQ \
	core mktape mktape.o \
	vtllib.o libvtlscsi.so \
	vtlcart.o spc.o \
	z.o z \
	TAGS \
	make_vtl_media \
	make_vtl_media.1 \
	vtllibrary vtllibrary.o

install:
	install -d -m 755 $(DESTDIR)$(PREFIX)/$(LIBDIR)
	install -o $(SUSER) -g bin -m 755 libvtlscsi.so $(DESTDIR)$(PREFIX)/$(LIBDIR)/
	install -d -m 755 $(DESTDIR)$(PREFIX)/bin
	install -o $(SUSER) -g $(GROUP) -m 4750 vtltape $(DESTDIR)$(PREFIX)/bin/
	install -o $(SUSER) -g $(GROUP) -m 4750 vtllibrary $(DESTDIR)$(PREFIX)/bin/
	install -o $(USR) -g $(GROUP) -m 750 vtlcmd $(DESTDIR)$(PREFIX)/bin/
	install -o $(USR) -g $(GROUP) -m 750 mktape $(DESTDIR)$(PREFIX)/bin/
	install -o $(USR) -g $(GROUP) -m 750 dump_tape $(DESTDIR)$(PREFIX)/bin/
	install -m 700 build_library_config $(DESTDIR)$(PREFIX)/bin/
	install -m 700 make_vtl_media $(DESTDIR)$(PREFIX)/bin/
	install -m 700 make_vtl_devices $(DESTDIR)$(PREFIX)/bin/

tar:
	make -C ../ tar

