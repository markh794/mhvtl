#
# etc/Makefile
#

CURDIR = "../"
include ../config.mk

MHVTL_CONF_FILE = mhvtl.conf
DEVICE_CONF_FILE = device.conf
LIB_CONTENTS_FILES = library_contents.10 library_contents.30
CONFIGS = $(MHVTL_CONF_FILE) $(DEVICE_CONF_FILE) $(LIB_CONTENTS_FILES)

# Static sample configs shipped as-is (no generation).
SAMPLE_CONFIGS = mhvtl_raw_bays.conf.sample

# tmpfiles.d snippet creating the runtime directories; installed as mhvtl.conf.
TMPFILES_SRC = mhvtl.tmpfiles

GENERATE_DEVICE_CONF = generate_device_conf
GENERATE_LIB_CONTENTS = generate_library_contents
GENERATORS = $(GENERATE_DEVICE_CONF) $(GENERATE_LIB_CONTENTS)

SERVICES = $(patsubst %.in,%,$(wildcard *.service.in))
MHVTL_TARGET = mhvtl.target


all: $(SERVICES) $(CONFIGS)

%: %.in
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' \
	    -e s'/@HOME_PATH@/$(HOME_PATH)/' \
	    -e s'/@LIBEXEC_PATH@/$(LIBEXEC_PATH)/' $< > $@

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
	$(RM) $(SERVICES) $(GENERATORS) $(MHVTL_CONF_FILE)
	$(RM) $(DEVICE_CONF_FILE) $(LIB_CONTENTS_FILES)

$(DESTDIR)$(SYSTEMD_SERVICE_DIR):
	install -d -m 755 $@
$(DESTDIR)$(MHVTL_CONFIG_PATH):
	install -d -m 755 $@
$(DESTDIR)$(PREFIX)/bin:
	install -d -m 755 $@
$(DESTDIR)$(TMPFILES_DIR):
	install -d -m 755 $@
$(DESTDIR)$(MHVTL_DATADIR):
	install -d -m 755 $@

# Installing services
$(DESTDIR)$(SYSTEMD_SERVICE_DIR)/%.service: %.service | $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	install -m 644 $< $@
	$(call SEMANAGE_UNIT,$@)
$(DESTDIR)$(SYSTEMD_SERVICE_DIR)/%.target: %.target | $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	install -m 644 $< $@
	$(call SEMANAGE_UNIT,$@)
# Installing generator of config files
$(DESTDIR)$(PREFIX)/bin/generate_%: generate_% | $(DESTDIR)$(PREFIX)/bin
	install -m 755 $< $@
# save backup copies of config files
$(DESTDIR)$(MHVTL_CONFIG_PATH)/%: % | $(DESTDIR)$(MHVTL_CONFIG_PATH)
	install -m 644 -b -S .orig $< $@
# tmpfiles.d snippet, installed as <tmpfilesdir>/mhvtl.conf
$(DESTDIR)$(TMPFILES_DIR)/mhvtl.conf: $(TMPFILES_SRC) | $(DESTDIR)$(TMPFILES_DIR)
	install -m 644 $< $@
# read-only sample configuration (vendor examples)
$(DESTDIR)$(MHVTL_DATADIR)/%: % | $(DESTDIR)$(MHVTL_DATADIR)
	install -m 644 $< $@

.PHONY: install
install: all \
		 $(addprefix $(DESTDIR)$(SYSTEMD_SERVICE_DIR)/,$(SERVICES) $(MHVTL_TARGET)) \
		 $(addprefix $(DESTDIR)$(MHVTL_CONFIG_PATH)/,$(CONFIGS)) \
		 $(addprefix $(DESTDIR)$(MHVTL_DATADIR)/,$(SAMPLE_CONFIGS)) \
		 $(addprefix $(DESTDIR)$(PREFIX)/bin/,$(GENERATORS)) \
		 $(DESTDIR)$(TMPFILES_DIR)/mhvtl.conf
	$(call RESTORECON_TREE,$(SYSTEMD_SERVICE_DIR))

# The configuration under $(MHVTL_CONFIG_PATH) describes the libraries the user
# built, so it is kept unless PURGE_CONFIG is set.
.PHONY: uninstall
uninstall:
	$(RM) $(addprefix $(DESTDIR)$(SYSTEMD_SERVICE_DIR)/,$(SERVICES) $(MHVTL_TARGET))
	$(RM) $(DESTDIR)$(TMPFILES_DIR)/mhvtl.conf
	$(RM) $(addprefix $(DESTDIR)$(MHVTL_DATADIR)/,$(SAMPLE_CONFIGS))
	$(RM) $(addprefix $(DESTDIR)$(PREFIX)/bin/,$(GENERATORS))
ifneq ($(PREFIX)/bin,/usr/bin)
	# Earlier releases installed the generators into /usr/bin regardless of PREFIX
	$(RM) $(addprefix $(DESTDIR)/usr/bin/,$(GENERATORS))
endif
	if $(HAVE_SEMANAGE); then \
		for unit in $(SERVICES) $(MHVTL_TARGET); do \
			semanage fcontext -d "$(SYSTEMD_SERVICE_DIR)/$$unit" || true; \
		done; \
	fi
ifeq ($(PURGE_CONFIG),)
	@echo "Left in place: $(DESTDIR)$(MHVTL_CONFIG_PATH) (use PURGE_CONFIG=1 to remove)"
else
	$(RM) $(addprefix $(DESTDIR)$(MHVTL_CONFIG_PATH)/,$(CONFIGS))
	$(RM) $(addsuffix .orig,$(addprefix $(DESTDIR)$(MHVTL_CONFIG_PATH)/,$(CONFIGS)))
endif
