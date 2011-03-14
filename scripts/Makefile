
RCFILE = update_device.conf
PREFIX ?= /usr
USR ?= vtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl

CONFIG_PATH = $(shell echo $(MHVTL_CONFIG_PATH) | sed -e s'/\//\\\//g')
$(RCFILE): $(RCFILE).in
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' $< > $@

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
	install -m 700 $(RCFILE) $(DESTDIR)$(PREFIX)/bin/

