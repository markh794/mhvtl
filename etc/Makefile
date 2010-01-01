
RCFILE = mhvtl
USR ?= vtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl

CONFIG_PATH = $(shell echo $(MHVTL_CONFIG_PATH) | sed -e s'/\//\\\//g')
$(RCFILE): $(RCFILE).in
	sed -e s'/@USR@/$(USR)/' $< > $@.1
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' $@.1 > $@
	rm -f $@.1

.PHONY: rcfile
rcfile: $(RCFILE)

.PHONY: distclean
distclean:
	rm -f $(RCFILE)

.PHONY: clean
clean:
	rm -f $(RCFILE)

.PHONY: install
install:
	install -d -m 755 /etc
	install -d -m 755 /etc/init.d
	install -m 700 $(RCFILE) /etc/init.d/
	insserv $(RCFILE)

