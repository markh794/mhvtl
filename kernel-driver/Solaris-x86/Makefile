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
# CC must be set to the Ansi-C compiler installed on the target
# system, e.g. CC=/opt/ansic/bin/cc
#
# $Id: Makefile,v 1.2.2.1 2006-08-06 07:59:05 markh Exp $
#

# CC=/usr/local/bin/gcc
# CFLAGS=-D_KERNEL -ffreestanding -nodefaultlibs -I/d/01/sol10/usr/src/uts/common
CC=/opt/SUNWspro/bin/cc
CFLAGS=-D_KERNEL -I/d/01/sol10/usr/src/uts/common

all:	vx_tape

vx_tape_bsd.o:	vx_tape_bsd.c
	$(CC) $(CFLAGS) -c vx_tape_bsd.c

vx_tape.o:	vx_tape.c
	$(CC) $(CFLAGS) -c vx_tape.c

vx_tape:	vx_tape.o vx_tape_bsd.o
	$(LD) -r -o vx_tape vx_tape.o vx_tape_bsd.o

clean:
	rm -f vx_tape.o vx_tape_bsd.o

distclean:
	rm -f vx_tape.o vx_tape_bsd.o vx_tape

install:
	install -o root -g bin -m 755 vx_tape /usr/kernel/drv/

