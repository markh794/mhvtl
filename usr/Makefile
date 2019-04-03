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
SYSTEMD_GENERATOR_DIR ?= /usr/lib/systemd/system-generators
MHVTL_HOME_PATH ?= /opt/mhvtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl
CONFIG_PATH = $(shell echo $(MHVTL_CONFIG_PATH) | sed -e s'/\//\\\//g')
HOME_PATH = $(shell echo $(MHVTL_HOME_PATH) | sed -e s'/\//\\\//g')

ifeq ($(shell grep lib64$ /etc/ld.so.conf /etc/ld.so.conf.d/* | wc -l),0)
LIBDIR ?= $(PREFIX)/lib
else
LIBDIR ?= $(PREFIX)/lib64
endif

CFLAGS=-Wall -Wshadow -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS) -I../kernel
CFLAGS += -DMHVTL_VERSION=\"$(VERSION)$(EXTRAVERSION)\"
CFLAGS += -D_GNU_SOURCE
CFLAGS += -DMHVTL_DEBUG
CFLAGS += -DMHVTL_HOME_PATH=\"$(MHVTL_HOME_PATH)\"
CFLAGS += -DMHVTL_CONFIG_PATH=\"$(MHVTL_CONFIG_PATH)\"

CLFLAGS=-shared ${RPM_OPT_FLAGS}

# Enable LZODEBUG
#LZODEBUG = -DLZO_DEBUG
LZODEBUG =

# files that need to be generated
GENERATOR_FILES=make_vtl_media.in
GENERATED_FILES=$(GENERATOR_FILES:.in=)

all:	libvtlscsi.so libvtlcart.so vtltape dump_tape vtlcmd dump_messageQ \
	mktape edit_tape vtllibrary tapeexerciser $(GENERATED_FILES) \
	mhvtl-device-conf-generator

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

minilzo.o:	minilzo.c lzoconf.h lzodefs.h
	$(CC) $(CFLAGS) $(LZODEBUG) -o minilzo.o -c minilzo.c

dump_tape:	dump_tape.o vtlcart.o libvtlscsi.so vtltape.h scsi.h \
		../kernel/vtl_common.h minilzo.c minilzo.h minilzo.o
	$(CC) $(CFLAGS) -o dump_tape dump_tape.o minilzo.o -L. \
		-lz -L. -lvtlcart -lvtlscsi

mktape:		mktape.c mktape.o vtlcart.o libvtlscsi.so vtltape.h vtllib.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o mktape mktape.o -L. -lvtlcart -lvtlscsi

edit_tape:	edit_tape.o vtlcart.o libvtlscsi.so vtltape.h vtllib.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o edit_tape edit_tape.o -L. -lvtlcart -lvtlscsi

vtllibrary:	vtllibrary.o vtllib.h scsi.h \
		libvtlscsi.so \
		vtl_cart_type.o \
		be_byteshift.h \
		stklxx_pm.o \
		hp_smc_pm.o \
		overland_pm.o \
		spectra_pm.o \
		scalar_pm.o \
		ibm_smc_pm.o \
		default_smc_pm.o \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o vtllibrary vtllibrary.o -L. -lvtlscsi \
		vtl_cart_type.o \
		stklxx_pm.o \
		hp_smc_pm.o \
		overland_pm.o \
		spectra_pm.o \
		scalar_pm.o \
		ibm_smc_pm.o \
		default_smc_pm.o

vtltape:	vtltape.o vtlcart.o vtllib.h vtltape.h scsi.h \
		libvtlscsi.so ssc.o default_ssc_pm.o \
		ult3580_pm.o \
		hp_ultrium_pm.o \
		stk9x40_pm.o \
		quantum_dlt_pm.o \
		ait_pm.o t10000_pm.o ibm_03592_pm.o \
		minilzo.o \
		be_byteshift.h \
		../kernel/vtl_common.h
	$(CC) $(CFLAGS) -o vtltape vtltape.o ssc.o \
		default_ssc_pm.o \
		ult3580_pm.o \
		hp_ultrium_pm.o \
		stk9x40_pm.o \
		quantum_dlt_pm.o \
		ait_pm.o t10000_pm.o ibm_03592_pm.o \
		minilzo.o \
		-lz -L. -lvtlcart -lvtlscsi

mhvtl-device-conf-generator: mhvtl-device-conf-generator.c

make_vtl_media: make_vtl_media.in
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' \
	    -e s'/@HOME_PATH@/$(HOME_PATH)/' $< > $@
	chmod 755 $@

clean:
	rm -f vtltape.o dump_tape.o q.o \
		vtlcmd.o dump_messageQ.o core mktape.o vtllib.o \
		libvtlscsi.o libvtlscsi.so libvtlcart.o libvtlcart.so \
		vtllibrary.o \
		vtl_cart_type.o \
		vtlcart.o spc.o smc.o ssc.o tapeexerciser.o \
		default_ssc_pm.o \
		ult3580_pm.o \
		hp_ultrium_pm.o \
		mode.o \
		log.o \
		subprocess.o \
		stk9x40_pm.o \
		stklxx_pm.o \
		hp_smc_pm.o \
		overland_pm.o \
		spectra_pm.o \
		scalar_pm.o \
		ibm_smc_pm.o \
		default_smc_pm.o \
		quantum_dlt_pm.o \
		ait_pm.o t10000_pm.o ibm_03592_pm.o \
		minilzo.o \
		edit_tape.o \
		dump_messageQ \
		dump_tape edit_tape \
		mktape vtlcmd vtllibrary vtltape tapeexerciser \
		mhvtl-device-conf-generator \
		$(GENERATED_FILES)
	$(RM) TAGS

tags:
	etags -R *.[ch] ../kernel/*.h

distclean: clean

install: all
	[ -d $(DESTDIR)$(LIBDIR) ] || install -d -m 755 $(DESTDIR)$(LIBDIR)
	install -m 755 libvtlscsi.so $(DESTDIR)$(LIBDIR)/
	install -m 755 libvtlcart.so $(DESTDIR)$(LIBDIR)/
	[ -d $(DESTDIR)$(PREFIX)/bin ] || install -d -m 755 $(DESTDIR)$(PREFIX)/bin
	install -m 755 vtltape $(DESTDIR)$(PREFIX)/bin/
	install -m 755 vtllibrary $(DESTDIR)$(PREFIX)/bin/
	install -m 755 vtlcmd $(DESTDIR)$(PREFIX)/bin/
	install -m 755 mktape $(DESTDIR)$(PREFIX)/bin/
	install -m 755 dump_tape $(DESTDIR)$(PREFIX)/bin/
	install -m 755 edit_tape $(DESTDIR)$(PREFIX)/bin/
	install -m 755 tapeexerciser $(DESTDIR)$(PREFIX)/bin/
	install -m 755 make_vtl_media $(DESTDIR)$(PREFIX)/bin/
	[ -d $(DESTDIR)$(SYSTEMD_GENERATOR_DIR) ] || install -d -m 755 $(DESTDIR)$(SYSTEMD_GENERATOR_DIR)
	install -m 755 mhvtl-device-conf-generator $(DESTDIR)$(SYSTEMD_GENERATOR_DIR)/

tar:
	make -C ../ tar

