#
# etc/Makefile
#

CURDIR = "../"
include ../config.mk

TAPE_SERVICE_FILE = vtltape@.service
LIB_SERVICE_FILE = vtllibrary@.service
MHVTL_TARGET_FILE = mhvtl.target
MHVTL_LOAD_MODULES_SERVICE_FILE = mhvtl-load-modules.service
MHVTL_CONF_FILE = mhvtl.conf
DEVICE_CONF_FILE = device.conf
LIB_CONTENTS_FILES = library_contents.10 library_contents.30

GENERATE_DEVICE_CONF = generate_device_conf
GENERATE_LIB_CONTENTS = generate_library_contents

# files that need to be generated
GENERATOR_FILES=$(MHVTL_CONF_FILE).in \
		$(TAPE_SERVICE_FILE).in \
		$(LIB_SERVICE_FILE).in \
		$(MHVTL_LOAD_MODULES_SERVICE_FILE).in \
		$(GENERATE_DEVICE_CONF).in \
		$(GENERATE_LIB_CONTENTS).in
GENERATED_FILES=$(GENERATOR_FILES:.in=)

ALL_FILES = $(GENERATED_FILES) $(DEVICE_CONF_FILE) $(LIB_CONTENTS_FILES)

all: $(ALL_FILES)

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

.PHONY: install
install: all
	[ -d $(DESTDIR)$(SYSTEMD_SERVICE_DIR) ] || install -d -m 755 $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	install -m 644 $(LIB_SERVICE_FILE) $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	semanage fcontext -a -t systemd_unit_file_t \'$(DESTDIR)$(SYSTEMD_SERVICE_DIR)/$(LIB_SERVICE_FILE)\'
	install -m 644 $(TAPE_SERVICE_FILE) $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	semanage fcontext -a -t systemd_unit_file_t \'$(DESTDIR)$(SYSTEMD_SERVICE_DIR)/$(TAPE_SERVICE_FILE)\'
	install -m 644 $(MHVTL_TARGET_FILE) $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	semanage fcontext -a -t systemd_unit_file_t \'$(DESTDIR)$(SYSTEMD_SERVICE_DIR)/$(MHVTL_TARGET_FILE)\'
	install -m 644 $(MHVTL_LOAD_MODULES_SERVICE_FILE) $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	semanage fcontext -a -t systemd_unit_file_t \'$(DESTDIR)$(SYSTEMD_SERVICE_DIR)/$(MHVTL_LOAD_MODULES_SERVICE_FILE)\'
	restorecon -R -v  $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	[ -d $(DESTDIR)$(MHVTL_CONFIG_PATH) ] || install -d -m 755 $(DESTDIR)$(MHVTL_CONFIG_PATH)
	install -m 755 $(GENERATE_DEVICE_CONF) $(DESTDIR)/usr/bin
	install -m 755 $(GENERATE_LIB_CONTENTS) $(DESTDIR)/usr/bin
	# save backup copies of config files
	install -m 644 -b -S .orig $(MHVTL_CONF_FILE) $(DESTDIR)$(MHVTL_CONFIG_PATH)
	install -m 644 -b -S .orig $(DEVICE_CONF_FILE) $(DESTDIR)$(MHVTL_CONFIG_PATH)
	install -m 644 -b -S .orig $(LIB_CONTENTS_FILES) $(DESTDIR)$(MHVTL_CONFIG_PATH)
