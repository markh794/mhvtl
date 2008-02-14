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

# Linux
CC= /usr/bin/gcc
# CFLAGS=-O2 -Wall -mcpu=pentium4 -march=i686
#CFLAGS=-O2 -Wall
CFLAGS=-Wall -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS)

CLFLAGS=-shared

all:	libvtlscsi.so vtltape dump_tape vtlcmd dump_messageQ mktape \
	vtllibrary vtl_set_sn

libvtlscsi.so:	vxshared.c vx.h vxshared.h scsi.h
	$(CC) $(CFLAGS) -c -fpic vxshared.c
	$(CC) $(CLFLAGS) -o libvtlscsi.so vxshared.o

dump_messageQ:	dump_messageQ.c q.h
	$(CC) $(CFLAGS) -o dump_messageQ dump_messageQ.c

vtl_set_sn:	vtl_set_sn.o vxshared.h vx.h scsi.h
	$(CC) $(CFLAGS) -o vtl_set_sn vtl_set_sn.o -L. -lvtlscsi

vtlcmd:	vtlcmd.o q.o q.h
	$(CC) $(CFLAGS) -o vtlcmd q.o vtlcmd.o

dump_tape:	dump_tape.o vx.h vtltape.h scsi.h
	$(CC) $(CFLAGS) -o dump_tape dump_tape.o

mktape:	mktape.o vtltape.h q.h vx.h vxshared.h
	$(CC) $(CFLAGS) -o mktape mktape.o


vtllibrary:	vtllibrary.o q.h vx.h vxshared.o vxshared.h scsi.h
	$(CC) $(CFLAGS) -o vtllibrary vtllibrary.o -L. -lvtlscsi

vtltape:	vtltape.o q.h vx.h vxshared.o vxshared.h vtltape.h scsi.h
	$(CC) $(CFLAGS) -o vtltape vtltape.o -L. -lz -lvtlscsi


clean:
	rm -f vtltape.o dump_tape.o q.o \
		vtlcmd.o q.o dump_messageQ.o core mktape.o \
		vxshared.o libvtlscsi.so z.o vtllibrary.o \
		vtl_set_sn.o

tags:
	etags *.c *.h

distclean:
	rm -f vtltape.o vtltape \
	dump_tape.o dump_tape \
	q.o q \
	vtlcmd.o vtlcmd \
	vtl_set_sn.o vtl_set_sn \
	dump_messageQ.o dump_messageQ \
	core mktape mktape.o \
	vxshared.o libvtlscsi.so \
	z.o z \
	TAGS \
	vtllibrary vtllibrary.o

install:
	install -o root -g bin -m 755 libvtlscsi.so /usr/lib/
	install -o vtl -g vtl -m 750 vtltape /usr/bin/
	install -o vtl -g vtl -m 750 vtllibrary /usr/bin/
	install -o vtl -g vtl -m 750 vtlcmd /usr/bin/
	install -o vtl -g vtl -m 750 vtl_set_sn /usr/bin/
	install -o vtl -g vtl -m 750 mktape /usr/bin/
	install -m 700 build_library_config /usr/bin/
	install -m 700 make_vtl_devices /usr/bin/
	install -m 700 vtl /etc/init.d/

tar:
	(cd ..;  tar cvfz /home/markh/vtl-`date +%F`.tgz  --exclude=.git vtl-0.14/man vtl-0.14/doc vtl-0.14/kernel-driver vtl-0.14/*.[ch] vtl-0.14/Makefile vtl-0.14/README vtl-0.14/INSTALL vtl-0.14/build_library_config vtl-0.14/library_contents.sample vtl-0.14/make_vtl_devices vtl-0.14/vtl.spec vtl-0.14/vtl)

