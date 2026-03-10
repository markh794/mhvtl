
CURDIR = "../"
include ../config.mk

RCFILE = update_device.conf

$(RCFILE): $(RCFILE).in
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' $< > $@

.PHONY: rcfile
rcfile: $(RCFILE)


.PHONY: install
install:
	install -m 700 $(RCFILE) $(DESTDIR)$(PREFIX)/bin/

.PHONY: uninstall
uninstall:
	$(RM)  $(DESTDIR)$(PREFIX)/bin/$(RCFILE)

# ========== Cleaning ==========

.PHONY: clean
clean:
	rm -f $(RCFILE)

.PHONY: distclean
distclean:
	rm -f $(RCFILE)
