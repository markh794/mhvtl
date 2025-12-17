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

CURDIR = "../"

include ../config.mk

MONTH = $(shell date -r ../ChangeLog +%B)
YEAR = $(shell date -r ../ChangeLog +%Y)

MAN_PATH = $(DESTDIR)$(PREFIX)$(MANDIR)
MAN1_PAGES = $(patsubst %.1.in,%.1,$(wildcard *.1.in))
MAN5_PAGES = $(patsubst %.5.in,%.5,$(wildcard *.5.in))

all: $(MAN1_PAGES) $(MAN5_PAGES)

# Create man pages in local folder
%: %.in
	sed -e s'/@VERSION@/$(VERSION)/' \
		-e s'/@MONTH@/$(MONTH)/' \
		-e s'/@YEAR@/$(YEAR)/' \
		-e s'/@CONFIG_PATH@/$(CONFIG_PATH)/' \
		-e s'/@HOME_PATH@/$(HOME_PATH)/' $< > $@

# Directories
$(MAN_PATH)/man1:
	install -d -m 755 $@
$(MAN_PATH)/man5:
	install -d -m 755 $@

# installation
$(MAN_PATH)/man1/%.1: %.1 | $(MAN_PATH)/man1
	install $< $@
$(MAN_PATH)/man5/%.5: %.5 | $(MAN_PATH)/man5
	install $< $@

.PHONY:install
install: $(addprefix $(MAN_PATH)/man1/,$(MAN1_PAGES)) \
		 $(addprefix $(MAN_PATH)/man5/,$(MAN5_PAGES))

# ========== Cleaning ==========

.PHONY:clean
clean:
	rm -f *.1 *.5
