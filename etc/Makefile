
RCFILE = mhvtl
MHVTL_CONFIG_PATH ?= /etc/mhvtl
MHVTL_HOME_PATH ?= /opt/mhvtl
INITD ?= /etc/init.d

CONFIG_PATH = $(shell echo $(MHVTL_CONFIG_PATH) | sed -e s'/\//\\\//g')
HOME_PATH = $(shell echo $(MHVTL_HOME_PATH) | sed -e s'/\//\\\//g')

$(RCFILE): $(RCFILE).in
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' $@.1 > $@
	sed -i s'/@HOME_PATH@/$(HOME_PATH)/' $@
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
	install -d -m 755 $(DESTDIR)$(INITD)
	install -m 700 $(RCFILE) $(DESTDIR)$(INITD)
	test ! -x /sbin/insserv || /sbin/insserv $(RCFILE)

