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

VER = $(shell grep Version mhvtl.spec|awk '{print $$2}')
REL = $(shell grep Release mhvtl.spec|awk '{print $$2}')

VERSION ?= $(VER).$(REL)
EXTRAVERSION =  $(if $(shell git-show-ref 2>/dev/null),-git-$(shell git-show-ref --head --abbrev|head -1|awk '{print $$1}'))

PARENTDIR = mhvtl-0.16
PREFIX ?= /usr
export PREFIX DESTDIR

CFLAGS=-Wall -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS)
CLFLAGS=-shared

all:	usr

usr:	patch
	$(MAKE) -C usr

kernel: patch
	$(MAKE) -C kernel

tags:
	$(MAKE) -C usr tags
	$(MAKE) -C kernel tags

patch:

clean:
	rm -f usr/*.o

distclean:
	$(MAKE) -C usr distclean
	$(MAKE) -C kernel distclean

install:
	$(MAKE) -C usr
	$(MAKE) -C usr install $(PREFIX) $(DESTDIR)

tar:
	$(MAKE) distclean
	(cd ..;  tar cvfz /home/markh/mhvtl-`date +%F`-$(VERSION)$(EXTRAVERSION).tgz  --exclude=.git \
		 $(PARENTDIR)/man \
		 $(PARENTDIR)/doc \
		 $(PARENTDIR)/kernel \
		 $(PARENTDIR)/usr \
		 $(PARENTDIR)/etc \
		 $(PARENTDIR)/include \
		 $(PARENTDIR)/Makefile \
		 $(PARENTDIR)/README \
		 $(PARENTDIR)/INSTALL \
		 $(PARENTDIR)/mhvtl.spec)

