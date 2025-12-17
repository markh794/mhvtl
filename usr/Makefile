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
GENERATOR_FILES=make_vtl_media.in mhvtl_kernel_mod_build.in
GENERATED_FILES=$(GENERATOR_FILES:.in=)
GENERATED_FILES:=$(addprefix bin/, $(GENERATED_FILES))

all: create_folders libvtlcart.so libvtlscsi.so commands validate_crc

commands : bin/vtltape bin/vtllibrary bin/vtlcmd bin/dump_tape bin/preload_tape bin/dump_messageQ \
	bin/mktape bin/edit_tape  bin/tapeexerciser \
	$(GENERATED_FILES)  \
	bin/mhvtl-device-conf-generator

create_folders :
	- mkdir bin
# ================== objects ==================

# ------------ VTL objects
VTL_SRC = $(wildcard ./*.c)
VTL_DEP = $(VTL_SRC:.c=.d)
-include $(VTL_DEP)
%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

mhvtl_io.o: \
	CFLAGS += -std=gnu99

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
	CFLAGS += -fPIC -std=gnu99


# ------------ utils
UTILS_SRC = $(wildcard utils/*.c)
UTILS_DEP = $(UTILS_SRC:.c=.d)
-include $(UTILS_DEP)
utils/%.o: utils/%.c
	$(CC) $(CFLAGS) -o $@ -c $< 


utils/crc32c.o \
utils/reed-solomon.o \
utils/validate_crc.o: \
		CFLAGS += -std=gnu99

utils/minilzo.o: \
		CFLAGS += $(LZODEBUG)

utils/q.o utils/subprocess.o: \
		CFLAGS += -fpic


# ================== libs ==================

libvtlscsi.so: vtllib.o mhvtl_log.o mode.o \
	 	spc.o smc.o \
	 	utils/q.o \
	 	utils/subprocess.o
	$(CC) $(CLFLAGS) -o $@ $^ -lpthread

libvtlcart.so: vtllib.o vtlcart.o mhvtl_log.o \
		utils/q.o 
	$(CC) $(CLFLAGS) -o $@ $^ -lpthread

# ================== Commands and scripts ==================

bin/validate_crc: utils/validate_crc.o utils/crc32c.o utils/reed-solomon.o
	@$(CC) $(CFLAGS) -o $@ $^ -std=gnu99

validate_crc: bin/validate_crc
	@./bin/validate_crc

bin/tapeexerciser: cmd/tapeexerciser.o
	$(CC) $(CFLAGS) -o $@ $^

DUMP_MESSAGEQ_OBJ = cmd/dump_messageQ.o
bin/dump_messageQ: $(DUMP_MESSAGEQ_OBJ) libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(DUMP_MESSAGEQ_OBJ) -L. -lvtlscsi

VTLCMD_OBJ = cmd/vtlcmd.o
bin/vtlcmd:	$(VTLCMD_OBJ) libvtlcart.so libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(VTLCMD_OBJ) -L. -lvtlcart -lvtlscsi

bin/preload_tape: bin/dump_tape
	@rm -f $@
	ln -s $? $@

DUMP_TAPE_OBJ = cmd/tape_util.o \
		mhvtl_io.o \
		utils/minilzo.o \
		utils/crc32c.o \
		utils/reed-solomon.o \
		pm/default_ssc_pm.o
bin/dump_tape: $(DUMP_TAPE_OBJ) libvtlcart.so libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(DUMP_TAPE_OBJ) -L. -lz -lvtlcart -lvtlscsi
		
MKTAPE_OBJ = cmd/mktape.o
bin/mktape: $(MKTAPE_OBJ) libvtlcart.so libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(MKTAPE_OBJ) -L. -lvtlcart -lvtlscsi

EDIT_TAPE_OBJ = cmd/edit_tape.o
bin/edit_tape: $(EDIT_TAPE_OBJ) libvtlcart.so libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(EDIT_TAPE_OBJ) -L. -lvtlcart -lvtlscsi

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
bin/vtltape: $(VTLTAPE_OBJ) libvtlcart.so libvtlscsi.so
	$(CC) $(CFLAGS) -o $@ $(VTLTAPE_OBJ) -lz -L. -lvtlcart -lvtlscsi

bin/mhvtl-device-conf-generator: cmd/mhvtl-device-conf-generator.o
	$(CC) $(CFLAGS) -o $@ $^



bin/make_vtl_media: cmd/make_vtl_media.in
	sed -e s'/@CONF_PATH@/$(CONFIG_PATH)/' \
	    -e s'/@HOME_PATH@/$(HOME_PATH)/' $< > $@
	chmod 755 $@

bin/mhvtl_kernel_mod_build: cmd/mhvtl_kernel_mod_build.in
	sed -e s'|@FIRMWAREDIR@|${FIRMWAREDIR}|' $< > $@
	chmod 755 $@

clean:
	$(RM) *.d *.o *.so \
		pm/*.d pm/*.o \
		utils/*.d utils/*.o \
		cmd/*.d cmd/*.o \
		bin/*
	$(RM) TAGS

tags:
	etags -R *.[ch] ../kernel/*.h

distclean: clean

install: all
	[ -d $(DESTDIR)$(LIBDIR) ] || install -d -m 755 $(DESTDIR)$(LIBDIR)
	install -m 755 libvtlscsi.so $(DESTDIR)$(LIBDIR)/
	install -m 755 libvtlcart.so $(DESTDIR)$(LIBDIR)/
	[ -d $(DESTDIR)$(PREFIX)/bin ] || install -d -m 755 $(DESTDIR)$(PREFIX)/bin
	install -m 755 bin/vtltape $(DESTDIR)$(PREFIX)/bin/
	install -m 755 bin/vtllibrary $(DESTDIR)$(PREFIX)/bin/
	install -m 755 bin/vtlcmd $(DESTDIR)$(PREFIX)/bin/
	install -m 755 bin/mktape $(DESTDIR)$(PREFIX)/bin/
	install -m 755 bin/dump_tape $(DESTDIR)$(PREFIX)/bin/
	@rm -f $(DESTDIR)$(PREFIX)/bin/preload_tape
	ln $(DESTDIR)$(PREFIX)/bin/dump_tape $(DESTDIR)$(PREFIX)/bin/preload_tape 
	install -m 755 bin/edit_tape $(DESTDIR)$(PREFIX)/bin/
	install -m 755 bin/tapeexerciser $(DESTDIR)$(PREFIX)/bin/
	install -m 755 bin/make_vtl_media $(DESTDIR)$(PREFIX)/bin/
	install -m 755 bin/mhvtl_kernel_mod_build $(DESTDIR)$(PREFIX)/bin/
	[ -d $(DESTDIR)$(SYSTEMD_GENERATOR_DIR) ] || install -d -m 755 $(DESTDIR)$(SYSTEMD_GENERATOR_DIR)
	install -m 755 bin/mhvtl-device-conf-generator $(DESTDIR)$(SYSTEMD_GENERATOR_DIR)/


tar:
	make -C ../ tar
