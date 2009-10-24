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

PREFIX ?= /usr
USER = vtl
SUSER = root
GROUP = vtl

CFLAGS=-Wall -Wshadow -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS) -I../kernel
CFLAGS += -DMHVTL_VERSION=\"$(VERSION)$(EXTRAVERSION)\"
CFLAGS += -DMHVTL_DEBUG
CFLAGS += -DUSER=\"$(USER)\"

CLFLAGS=-shared ${RPM_OPT_FLAGS}

all:	libvtlscsi.so vtltape dump_tape vtlcmd dump_messageQ mktape \
	vtllibrary

libvtlscsi.so:	vtllib.c spc.c vtllib.h scsi.h \
		be_byteshift.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -c -fpic vtllib.c
	$(CC) $(CFLAGS) -c -fpic spc.c
	$(CC) $(CLFLAGS) -o libvtlscsi.so vtllib.o spc.o

dump_messageQ:	dump_messageQ.c q.h
	$(CC) $(CFLAGS) -o dump_messageQ dump_messageQ.c

vtlcmd:	vtlcmd.o q.o q.h
	$(CC) $(CFLAGS) -o vtlcmd q.o vtlcmd.o

dump_tape:	dump_tape.o vtltape.h scsi.h ../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o dump_tape dump_tape.o

mktape:	mktape.o vtltape.h q.h vtllib.h ../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o mktape mktape.o

vtllibrary:	vtllibrary.o q.h vtllib.o vtllib.h scsi.h \
		be_byteshift.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o vtllibrary vtllibrary.o -L. -lvtlscsi

vtltape:	vtltape.o q.h vtllib.o vtllib.h vtltape.h scsi.h \
		be_byteshift.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o vtltape vtltape.o -L. -lz -lvtlscsi

clean:
	rm -f vtltape.o dump_tape.o q.o \
		vtlcmd.o q.o dump_messageQ.o core mktape.o \
		vtllib.o libvtlscsi.so z.o vtllibrary.o \
		spc.o

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
	spc.o \
	z.o z \
	TAGS \
	vtllibrary vtllibrary.o

install:
	install -d -m 755 $(DESTDIR)$(PREFIX)/lib
	install -o $(SUSER) -g bin -m 755 libvtlscsi.so $(DESTDIR)$(PREFIX)/lib/
	install -d -m 755 $(DESTDIR)$(PREFIX)/bin
	install -o $(SUSER) -g $(USER) -m 4750 vtltape $(DESTDIR)$(PREFIX)/bin/
	install -o $(SUSER) -g $(USER) -m 4750 vtllibrary $(DESTDIR)$(PREFIX)/bin/
	install -o $(USER) -g $(GROUP) -m 750 vtlcmd $(DESTDIR)$(PREFIX)/bin/
	install -o $(USER) -g $(GROUP) -m 750 mktape $(DESTDIR)$(PREFIX)/bin/
	install -m 700 build_library_config $(DESTDIR)$(PREFIX)/bin/
	install -m 700 make_vtl_media $(DESTDIR)$(PREFIX)/bin/
	install -m 700 make_vtl_devices $(DESTDIR)$(PREFIX)/bin/
	install -d -m 755 $(DESTDIR)/etc
	install -d -m 755 $(DESTDIR)/etc/init.d
	install -m 700 ../etc/mhvtl $(DESTDIR)/etc/init.d/

tar:
	make -C ../ tar

