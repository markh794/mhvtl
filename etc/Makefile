#
# etc/Makefile
#

CURDIR = "../"
include ../config.mk

MHVTL_CONF_FILE = mhvtl.conf
DEVICE_CONF_FILE = device.conf
LIB_CONTENTS_FILES = library_contents.10 library_contents.30
CONFIGS = $(MHVTL_CONF_FILE) $(DEVICE_CONF_FILE) $(LIB_CONTENTS_FILES)

GENERATE_DEVICE_CONF = generate_device_conf
GENERATE_LIB_CONTENTS = generate_library_contents
GENERATORS = $(GENERATE_DEVICE_CONF) $(GENERATE_LIB_CONTENTS)

SERVICES = $(patsubst %.in,%,$(wildcard *.service*))
MHVTL_TARGET = mhvtl.target


all: $(SERVICES) $(CONFIGS)

%: %.in
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' \
	    -e s'/@HOME_PATH@/$(HOME_PATH)/' $< > $@

$(GENERATE_DEVICE_CONF): $(GENERATE_DEVICE_CONF).in
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' \
	    -e s'/@HOME_PATH@/$(HOME_PATH)/' $< > $@
	chmod 755 $@

$(GENERATE_LIB_CONTENTS): $(GENERATE_LIB_CONTENTS).in
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' \
	    -e s'/@HOME_PATH@/$(HOME_PATH)/' $< > $@
	chmod 755 $@

$(DEVICE_CONF_FILE): $(GENERATE_DEVICE_CONF)
	bash ./$(GENERATE_DEVICE_CONF) --force --home-dir=$(MHVTL_HOME_PATH) --override-home

$(LIB_CONTENTS_FILES): $(GENERATE_LIB_CONTENTS) $(DEVICE_CONF_FILE)
	bash ./$(GENERATE_LIB_CONTENTS) --force --config=.

.PHONY: distclean
distclean: clean

.PHONY: clean
clean:
	$(RM) $(GENERATED_FILES)
	$(RM) $(DEVICE_CONF_FILE) $(LIB_CONTENTS_FILES)

$(DESTDIR)$(SYSTEMD_SERVICE_DIR):
	install -d -m 755 $@
$(DESTDIR)$(MHVTL_CONFIG_PATH):
	install -d -m 755 $@

# Installing services
$(DESTDIR)$(SYSTEMD_SERVICE_DIR)/%.service: %.service | $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	install -m 644 $< $@
	semanage fcontext -a -t systemd_unit_file_t \'$@\'
$(DESTDIR)$(SYSTEMD_SERVICE_DIR)/%.target: %.target | $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	install -m 644 $< $@
	semanage fcontext -a -t systemd_unit_file_t \'$@\'
# Installing generator of config files
$(DESTDIR)/usr/bin/generate_%: generate_%
	install -m 755 $< $@
# save backup copies of config files
$(DESTDIR)$(MHVTL_CONFIG_PATH)/%: % | $(DESTDIR)$(MHVTL_CONFIG_PATH)
	install -m 644 -b -S .orig $< $@

.PHONY: install
install: all \
		 $(addprefix $(DESTDIR)$(SYSTEMD_SERVICE_DIR)/,$(SERVICES) $(MHVTL_TARGET)) \
		 $(addprefix $(DESTDIR)$(MHVTL_CONFIG_PATH)/,$(CONFIGS)) \
		 $(addprefix $(DESTDIR)/usr/bin/,$(GENERATORS))
	restorecon -R -v  $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
