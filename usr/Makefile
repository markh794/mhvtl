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
# $< is a variable that expands to the name of the source file
# @ at the beginning of the first line tell make not to echo the commands as it run it.
#

CURDIR = "../"

include ../config.mk

IFLAGS = -I ../include -I ../include/utils -I ../include/common -I ../ccan

CFLAGS = -Wall -Wshadow -g -O2 -D_LARGEFILE64_SOURCE $(RPM_OPT_FLAGS) $(IFLAGS)
CFLAGS += -MMD -MP # create header dependencies dynamically (.d files)
CFLAGS += -std=gnu99
CFLAGS += -DMHVTL_VERSION=\"$(VERSION).$(EXTRAVERSION)\"
CFLAGS += -DMHVTL_GITDATE=\"$(GITDATE)\"
CFLAGS += -DMHVTL_GITHASH=\"$(GITHASH)\"
CFLAGS += -D_GNU_SOURCE
CFLAGS += -DMHVTL_DEBUG
CFLAGS += -DMHVTL_HOME_PATH=\"$(MHVTL_HOME_PATH)\"
CFLAGS += -DMHVTL_CONFIG_PATH=\"$(MHVTL_CONFIG_PATH)\"
CFLAGS += -DSYSTEMD_SERVICE_DIR=\"$(SYSTEMD_SERVICE_DIR)\"

CLFLAGS = -shared ${RPM_OPT_FLAGS}

# Enable LZODEBUG
#LZODEBUG = -DLZO_DEBUG
LZODEBUG =

# files that need to be generated
GENERATED_FILES = $(patsubst cmd/%.in,bin/%,$(wildcard cmd/*.in))

BINARIES = $(patsubst cmd/%.c,bin/%,$(wildcard cmd/*.c)) bin/dump_tape
LIBRARIES = libvtlscsi.so

all: | bin
all: $(LIBRARIES) $(BINARIES) $(GENERATED_FILES) validate_crc

bin :
	install -d -m 755 $@
	
# ================== objects ==================

# ------------ VTL objects
VTL_SRC = $(wildcard ./*.c)
VTL_DEP = $(VTL_SRC:.c=.d)
-include $(VTL_DEP)
%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

mhvtl_log.o mode.o \
smc.o spc.o \
vtlcart.o vtllib.o: \
	CFLAGS += -fpic


# ------------ personality modules
PM_SRC = $(wildcard pm/*.c)
PM_DEP = $(PM_SRC:.c=.d)
-include $(PM_DEP)
pm/%.o: pm/%.c
	$(CC) $(CFLAGS) -o $@ -c $<


# ------------ cmds
CMD_SRC = $(wildcard cmd/*.c)
CMD_DEP = $(CMD_SRC:.c=.d)
-include $(CMD_DEP)
cmd/%.o: cmd/%.c
	$(CC) $(CFLAGS) -o $@ -c $<

cmd/tape_util.o: \
	CFLAGS += -fPIC


# ------------ utils
UTILS_SRC = $(wildcard utils/*.c)
UTILS_DEP = $(UTILS_SRC:.c=.d)
-include $(UTILS_DEP)
utils/%.o: utils/%.c
	$(CC) $(CFLAGS) -o $@ -c $< 

utils/minilzo.o: \
		CFLAGS += $(LZODEBUG)

utils/q.o utils/subprocess.o utils/mhvtl_update.o: \
		CFLAGS += -fpic


# ================== libs ==================

libvtlscsi.so: vtllib.o mhvtl_log.o mode.o \
		vtlcart.o \
	 	spc.o smc.o \
	 	utils/q.o \
	 	utils/subprocess.o \
		utils/mhvtl_update.o
	$(CC) $(CLFLAGS) -o $@ $^ -lpthread

# ================== Commands and scripts ==================

bin/validate_crc: utils/validate_crc.o utils/crc32c.o utils/reed-solomon.o
	@$(CC) $(CFLAGS) -o $@ $^

.PHONY: validate_crc
validate_crc: bin/validate_crc
	@./bin/validate_crc

bin/tapeexerciser: cmd/tapeexerciser.o
	$(CC) $(CFLAGS) -o $@ $^

DUMP_MESSAGEQ_OBJ = cmd/dump_messageQ.o
bin/dump_messageQ: $(DUMP_MESSAGEQ_OBJ) libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(DUMP_MESSAGEQ_OBJ) -L. -lvtlscsi

VTLCMD_OBJ = cmd/vtlcmd.o
bin/vtlcmd:	$(VTLCMD_OBJ) libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(VTLCMD_OBJ) -L. -lvtlscsi

bin/preload_tape: bin/dump_tape
	@rm -f $@
	ln -s $? $@

bin/tape_util:
	@true

DUMP_TAPE_OBJ = cmd/tape_util.o \
		mhvtl_io.o \
		utils/minilzo.o \
		utils/crc32c.o \
		utils/reed-solomon.o \
		pm/default_ssc_pm.o
bin/dump_tape: $(DUMP_TAPE_OBJ) libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(DUMP_TAPE_OBJ) -L. -lz -lvtlscsi
		
MKTAPE_OBJ = cmd/mktape.o
bin/mktape: $(MKTAPE_OBJ) libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(MKTAPE_OBJ) -L. -lvtlscsi

EDIT_TAPE_OBJ = cmd/edit_tape.o
bin/edit_tape: $(EDIT_TAPE_OBJ) libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(EDIT_TAPE_OBJ) -L. -lvtlscsi

VTLLIBRARY_OBJ = cmd/vtllibrary.o \
		vtl_cart_type.o \
		pm/stklxx_pm.o \
		pm/hp_smc_pm.o \
		pm/overland_pm.o \
		pm/spectra_pm.o \
		pm/scalar_pm.o \
		pm/ibm_smc_pm.o \
		pm/default_smc_pm.o
bin/vtllibrary:	$(VTLLIBRARY_OBJ) libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(VTLLIBRARY_OBJ) -L. -lvtlscsi

VTLTAPE_OBJ = cmd/vtltape.o \
		mhvtl_io.o ssc.o \
		utils/minilzo.o \
		utils/crc32c.o \
		utils/reed-solomon.o \
		pm/default_ssc_pm.o \
		pm/ult3580_pm.o \
		pm/hp_ultrium_pm.o \
		pm/stk9x40_pm.o \
		pm/quantum_dlt_pm.o \
		pm/ait_pm.o \
		pm/t10000_pm.o \
		pm/ibm_03592_pm.o
bin/vtltape: $(VTLTAPE_OBJ) libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(VTLTAPE_OBJ) -lz -L. -lvtlscsi

MHVTL_DEVICE_CONF_GENERATOR_OBJ = cmd/mhvtl-device-conf-generator.o
bin/mhvtl-device-conf-generator: $(MHVTL_DEVICE_CONF_GENERATOR_OBJ) libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(MHVTL_DEVICE_CONF_GENERATOR_OBJ) -L. -lvtlscsi



bin/make_vtl_media: cmd/make_vtl_media.in
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' \
	    -e s'/@HOME_PATH@/$(HOME_PATH)/' $< > $@
	chmod 755 $@

bin/mhvtl_kernel_mod_build: cmd/mhvtl_kernel_mod_build.in
	sed -e s'|@FIRMWAREDIR@|${FIRMWAREDIR}|' $< > $@
	chmod 755 $@

.PHONY:clean
clean:
	$(RM) *.d *.o *.so \
		pm/*.d pm/*.o \
		utils/*.d utils/*.o \
		cmd/*.d cmd/*.o \
		bin/*
	$(RM) TAGS



# ========== command targets ==========

$(DESTDIR)$(PREFIX)/bin:
	install -d -m 755 $@
$(DESTDIR)$(LIBDIR):
	install -d -m 755 $@

$(DESTDIR)$(LIBDIR)/%.so: %.so | $(DESTDIR)$(LIBDIR)
	install -m 755 $< $@
$(DESTDIR)$(PREFIX)/bin/%: bin/% | $(DESTDIR)$(PREFIX)/bin
	install -m 755 $< $@
$(DESTDIR)$(PREFIX)/bin/tape_util: bin/tape_util
	@true

.PHONY:install
install: all \
		 $(addprefix $(DESTDIR)$(PREFIX)/,$(BINARIES) $(GENERATED_FILES)) \
		 $(addprefix $(DESTDIR)$(LIBDIR)/,$(LIBRARIES)) 
	@rm -f $(DESTDIR)$(PREFIX)/bin/preload_tape
	ln $(DESTDIR)$(PREFIX)/bin/dump_tape $(DESTDIR)$(PREFIX)/bin/preload_tape
	[ -d $(DESTDIR)$(SYSTEMD_GENERATOR_DIR) ] || install -d -m 755 $(DESTDIR)$(SYSTEMD_GENERATOR_DIR)
	install -m 755 bin/mhvtl-device-conf-generator $(DESTDIR)$(SYSTEMD_GENERATOR_DIR)/
	restorecon -R -v  $(DESTDIR)$(SYSTEMD_GENERATOR_DIR)
ifeq ($(ROOTUID),YES)
	ldconfig 
endif

.PHONY: uninstall
uninstall:
	rm -f $(addprefix $(DESTDIR)$(PREFIX)/,$(BINARIES) $(GENERATED_FILES)) \
		  $(addprefix $(DESTDIR)$(LIBDIR)/,$(LIBRARIES)) \
		  $(DESTDIR)$(SYSTEMD_GENERATOR_DIR)/mhvtl-device-conf-generator
	restorecon -R -v  $(DESTDIR)$(SYSTEMD_GENERATOR_DIR)
ifeq ($(ROOTUID),YES)
	ldconfig 
endif	

.PHONY:tar
tar:
	make -C ../ tar

.PHONY:tags
tags:
	etags -R *.[ch] ../kernel/*.h

# ========== Cleaning ==========

.PHONY:clean
clean:
	$(RM) *.d *.o *.so \
		pm/*.d pm/*.o \
		utils/*.d utils/*.o \
		cmd/*.d cmd/*.o \
		bin/*
	$(RM) TAGS

.PHONY:distclean
distclean: clean