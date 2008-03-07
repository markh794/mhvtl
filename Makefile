#
# This makefile needs to be invoked as follows:
#
#make <options>
#
# Here, options include:
#
# 	all 	to build all utilities
# 	clean	to clean up all intermediate files
# 	kernel	to build kernel module
#

CFLAGS=-Wall -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS)
CLFLAGS=-shared

all:	usr

usr:	patch
	$(MAKE) -C usr

patch:

clean:
	rm -f usr/*.o

kernel: patch
	$(MAKE) -C kernel

tags:
	$(MAKE) -C usr tags

distclean:
	rm -f usr/*.o \
	usr/vtltape \
	usr/dump_tape \
	usr/vtlcmd \
	usr/vtl_set_sn \
	usr/dump_messageQ \
	usr/mktape mktape.o \
	usr/libvtlscsi.so \
	usr/z \
	usr/TAGS \
	usr/vtllibrary

install:
	install -o root -g bin -m 755 usr/libvtlscsi.so /usr/lib/
	install -o vtl -g vtl -m 750 usr/vtltape /usr/bin/
	install -o vtl -g vtl -m 750 usr/vtllibrary /usr/bin/
	install -o vtl -g vtl -m 750 usr/vtlcmd /usr/bin/
	install -o vtl -g vtl -m 750 usr/vtl_set_sn /usr/bin/
	install -o vtl -g vtl -m 750 usr/mktape /usr/bin/
	install -m 700 usr/build_library_config /usr/bin/
	install -m 700 usr/make_vtl_devices /usr/bin/
	install -m 700 etc/vtl /etc/init.d/

tar:
	(cd ..;  tar cvfz /home/markh/mhvtl-`date +%F`.tgz  --exclude=.git \
		 mhvtl-0.15/man \
		 mhvtl-0.15/doc \
		 mhvtl-0.15/kernel \
		 mhvtl-0.15/usr \
		 mhvtl-0.15/etc \
		 mhvtl-0.15/Makefile \
		 mhvtl-0.15/README \
		 mhvtl-0.15/INSTALL \
		 mhvtl-0.15/mhvtl.spec)

