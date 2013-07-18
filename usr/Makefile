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
REL = $(shell awk '/Release/ {print $$2}' ../mhvtl-utils.spec | sed 's/%{?dist}//g')

VERSION ?= $(VER).$(REL)
EXTRAVERSION =  $(if $(shell git show-ref 2>/dev/null),-git-$(shell git show-ref --head --abbrev|head -1|awk '{print $$1}'))

PREFIX ?= /usr
USR ?= vtl
SUSER = root
GROUP ?= vtl
MHVTL_HOME_PATH ?= /opt/mhvtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl
CONFIG_PATH = $(shell echo $(MHVTL_CONFIG_PATH) | sed -e s'/\//\\\//g')
HOME_PATH = $(shell echo $(MHVTL_HOME_PATH) | sed -e s'/\//\\\//g')

ifeq ($(shell uname -m),x86_64)
LIBDIR ?= $(PREFIX)/lib64
else
LIBDIR ?= $(PREFIX)/lib
endif

# Ubuntu / mint seem to have stopped using /lib64/
ifeq ($(shell grep lib64 /etc/ld.so.conf /etc/ld.so.conf.d/* | wc -l),0)
LIBDIR = $(PREFIX)/lib
endif

CFLAGS=-Wall -Wshadow -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS) -I../kernel
CFLAGS += -DMHVTL_VERSION=\"$(VERSION)$(EXTRAVERSION)\"
CFLAGS += -D_GNU_SOURCE
CFLAGS += -DMHVTL_DEBUG
CFLAGS += -DUSR=\"$(USR)\"
CFLAGS += -DMHVTL_HOME_PATH=\"$(MHVTL_HOME_PATH)\"
CFLAGS += -DMHVTL_CONFIG_PATH=\"$(MHVTL_CONFIG_PATH)\"

CLFLAGS=-shared ${RPM_OPT_FLAGS}

all:	libvtlscsi.so libvtlcart.so vtltape dump_tape vtlcmd dump_messageQ \
	mktape edit_tape vtllibrary make_vtl_media tapeexerciser

libvtlscsi.so:	vtllib.c spc.c vtllib.h scsi.h smc.c spc.c q.c \
		subprocess.c subprocess.h \
		mode.c log.c be_byteshift.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -c -fpic log.c
	$(CC) $(CFLAGS) -c -fpic mode.c
	$(CC) $(CFLAGS) -c -fpic vtllib.c
	$(CC) $(CFLAGS) -c -fpic spc.c
	$(CC) $(CFLAGS) -c -fpic smc.c
	$(CC) $(CFLAGS) -c -fpic q.c
	$(CC) $(CFLAGS) -c -fpic subprocess.c
	$(CC) $(CLFLAGS) -o libvtlscsi.so vtllib.o spc.o smc.o q.o \
		mode.o log.o subprocess.o -lpthread

libvtlcart.so: vtlcart.c vtllib.h vtllib.c scsi.h log.c q.c \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -c -fpic log.c
	$(CC) $(CFLAGS) -c -fpic q.c
	$(CC) $(CFLAGS) -c -fpic -o vtlcart.o vtlcart.c
	$(CC) $(CFLAGS) -c -fpic vtllib.c
	$(CC) $(CLFLAGS) -o libvtlcart.so vtllib.o vtlcart.o q.o log.o \
				-lpthread

tapeexerciser:	tapeexerciser.c
	$(CC) $(CFLAGS) -o tapeexerciser tapeexerciser.c

dump_messageQ:	libvtlscsi.so dump_messageQ.o
	$(CC) $(CFLAGS) -o dump_messageQ dump_messageQ.o -L. -lvtlscsi

vtlcmd:	vtlcmd.o
	$(CC) $(CFLAGS) -o vtlcmd vtlcmd.o -L. -lvtlcart -lvtlscsi

dump_tape:	dump_tape.o vtlcart.o libvtlscsi.so vtltape.h scsi.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o dump_tape dump_tape.o -L. -lvtlcart -lvtlscsi

mktape:		mktape.o vtlcart.o libvtlscsi.so vtltape.h vtllib.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o mktape mktape.o -L. -lvtlcart -lvtlscsi

edit_tape:	edit_tape.o vtlcart.o libvtlscsi.so vtltape.h vtllib.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o edit_tape edit_tape.o -L. -lvtlcart -lvtlscsi

vtllibrary:	vtllibrary.o vtllib.h scsi.h \
		libvtlscsi.so \
		be_byteshift.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o vtllibrary vtllibrary.o -L. -lvtlscsi

vtltape:	vtltape.o vtlcart.o vtllib.h vtltape.h scsi.h \
		libvtlscsi.so ssc.o default_ssc_pm.o \
		ult3580_pm.o \
		hp_ultrium_pm.o \
		stk9x40_pm.o \
		quantum_dlt_pm.o \
		ait_pm.o t10000_pm.o ibm_03592_pm.o \
		be_byteshift.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o vtltape vtltape.o ssc.o \
		default_ssc_pm.o \
		ult3580_pm.o \
		hp_ultrium_pm.o \
		stk9x40_pm.o \
		quantum_dlt_pm.o \
		ait_pm.o t10000_pm.o ibm_03592_pm.o \
		-lz -llzo2 -L. -lvtlcart -lvtlscsi

make_vtl_media:	make_vtl_media.in
	sed -e s'/@HOME_PATH@/$(HOME_PATH)/' $< > $@.1
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' $@.1 > $@
	rm -f $@.1

clean:
	rm -f vtltape.o dump_tape.o q.o \
		vtlcmd.o dump_messageQ.o core mktape.o \
		vtllib.o libvtlscsi.so libvtlcart.o z.o vtllibrary.o \
		vtlcart.o spc.o smc.o ssc.o tapeexerciser.o \
		default_ssc_pm.o \
		ult3580_pm.o \
		hp_ultrium_pm.o \
		mode.o \
		log.o \
		subprocess.o \
		stk9x40_pm.o \
		quantum_dlt_pm.o \
		ait_pm.o t10000_pm.o ibm_03592_pm.o \
		edit_tape.o \
		dump_messageQ make_vtl_media \
		dump_tape edit_tape \
		mktape vtlcmd vtllibrary vtltape tapeexerciser

tags:
	etags -R * ../kernel/*.h

distclean:
	rm -f vtltape.o vtltape \
	dump_tape.o dump_tape \
	edit_tape.o edit_tape \
	q.o q \
	vtlcmd.o vtlcmd \
	dump_messageQ.o dump_messageQ \
	core mktape mktape.o \
	vtllib.o libvtlscsi.so \
	libvtlcart.so vtlcart.o spc.o \
	smc.o ssc.o \
	default_ssc_pm.o \
	ult3580_pm.o \
	hp_ultrium_pm.o \
	stk9x40_pm.o \
	quantum_dlt_pm.o \
	ait_pm.o t10000_pm.o ibm_03592_pm.o \
	z.o z \
	mode.o \
	log.o \
	subprocess.o \
	TAGS \
	make_vtl_media \
	make_vtl_media.1 \
	tapeexerciser tapeexerciser.o \
	vtllibrary vtllibrary.o

install:
	install -d -m 755 $(DESTDIR)$(LIBDIR)
	install -o $(SUSER) -g bin -m 755 libvtlscsi.so $(DESTDIR)$(LIBDIR)/
	install -o $(SUSER) -g bin -m 755 libvtlcart.so $(DESTDIR)$(LIBDIR)/
	install -d -m 755 $(DESTDIR)$(PREFIX)/bin
	install -o $(SUSER) -g $(GROUP) -m 4750 vtltape $(DESTDIR)$(PREFIX)/bin/
	install -o $(SUSER) -g $(GROUP) -m 4750 vtllibrary $(DESTDIR)$(PREFIX)/bin/
	install -o $(USR) -g $(GROUP) -m 750 vtlcmd $(DESTDIR)$(PREFIX)/bin/
	install -o $(USR) -g $(GROUP) -m 750 mktape $(DESTDIR)$(PREFIX)/bin/
	install -o $(USR) -g $(GROUP) -m 750 dump_tape $(DESTDIR)$(PREFIX)/bin/
	install -o $(USR) -g $(GROUP) -m 750 edit_tape $(DESTDIR)$(PREFIX)/bin/
	install -o $(USR) -g $(GROUP) -m 755 tapeexerciser $(DESTDIR)$(PREFIX)/bin/
	install -m 700 build_library_config $(DESTDIR)$(PREFIX)/bin/
	install -m 700 make_vtl_media $(DESTDIR)$(PREFIX)/bin/

tar:
	make -C ../ tar

