#
# tests/Makefile — build and run mhvtl unit tests
#
# Each test_*.c compiles to a standalone binary. Tests are self-contained
# and pull in only the headers / source snippets they need (no dependency
# on the full usr/ library build).
#
# Usage:
#   make          — build all tests
#   make test     — build and run all tests
#   make clean    — remove binaries
#

CC      = gcc
CFLAGS  = -Wall -Wshadow -g -O2 -std=gnu99
CFLAGS += -D_GNU_SOURCE -D_LARGEFILE64_SOURCE

# Include paths matching the project layout
IFLAGS  = -I .
IFLAGS += -I ../include/utils
IFLAGS += -I ../include/common
IFLAGS += -I ../include
IFLAGS += -I ../ccan

# Sources compiled into test_crc
RS_SRC   = ../usr/utils/reed-solomon.c
CRC_SRC  = ../usr/utils/crc32c.c

# All test binaries
TESTS = test_byteshift test_crc test_sense test_smc_slots test_vpd test_log_volstat test_vtlcmd_parsing test_raw_bay_conf test_compression test_compression_select test_open_fifo test_q_reply test_q_send test_media_home test_dump_tape_output
TESTS += test_dump_tape_loop
TESTS += test_indx_rollback
TESTS += test_m8_mam

TESTS += test_raw_read_mam

TESTS += test_dump_tape_options

# Shell checks of the helper scripts; nothing to build
SCRIPT_TESTS = check_make_vtl_media.sh
SCRIPT_TESTS += make_vtl_media_home.sh
SCRIPT_TESTS += generator/run.sh

# Integration tests via userland transport (no kernel modules or root needed)
INTEGRATION_TESTS = test_userland_transport test_drive_types test_tape_io test_smc_dvcid test_queue_backoff test_ssc_fixed_ew test_ssc_erase test_read_sili test_ssc_space test_ssc_eom test_mode_sense_saved test_smc_load_failure test_conf_sscanf test_encryption_mode_page
INTEGRATION_TESTS += test_ssc_worm
INTEGRATION_TESTS += test_encryption_state
INTEGRATION_TESTS += test_lto_m8
INTEGRATION_TESTS += test_log_volstat_strings
INTEGRATION_TESTS += test_encryption_read_key
INTEGRATION_TESTS += test_ssc_load_unload

INTEGRATION_TESTS += test_smc_drive_absent
INTEGRATION_TESTS += test_log_volstat_len

.PHONY: all test clean

all: $(TESTS) $(INTEGRATION_TESTS)

test_byteshift: test_byteshift.c acutest.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_crc: test_crc.c acutest.h $(RS_SRC) $(CRC_SRC)
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_crc.c $(RS_SRC) $(CRC_SRC) -lpthread

test_sense: test_sense.c acutest.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_smc_slots: test_smc_slots.c acutest.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_vpd: test_vpd.c acutest.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_log_volstat: test_log_volstat.c acutest.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_raw_bay_conf: test_raw_bay_conf.c acutest.h ../usr/raw_bay_conf.c ../include/raw_bay_conf.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_raw_bay_conf.c ../usr/raw_bay_conf.c

# Compression codec round-trip (file backend here; the raw block-device backend
# is exercised by tests/test_raw_loopback.sh, which rebuilds this with
# -DRAW_BACKEND_AVAILABLE and links libvtlscsi for cart_backend_raw).
COMPRESS_SRC = ../usr/mhvtl_compress.c
test_compression: test_compression.c ../include/mhvtl_compress.h $(COMPRESS_SRC)
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_compression.c $(COMPRESS_SRC) -llz4 -lzstd

# Throughput benchmark (US-C3). Not a pass/fail test, so it is NOT in $(TESTS);
# build + run it with `make bench` (optionally `make bench BENCH_ARGS="512 256 1"`
# = total_MiB block_KiB zstd_level). Uses the same shared codec path as the
# write path; the file backend here, no root needed.
bench_compression: bench_compression.c
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ bench_compression.c -llzo2 -lz -llz4 -lzstd

.PHONY: bench
bench: bench_compression
	./bench_compression $(BENCH_ARGS)

# Runtime codec selection (US-C2). Links libvtlscsi for the real mode-page and
# tape-write entry points (set_mode_compression / load_tape / write_tape_block)
# plus mhvtl_compress.c for the codec helpers. The selection + MODE SENSE
# reflection checks run here with no backend; the write-path flag check runs
# when tests/test_raw_loopback.sh passes a block device + barcode.
../usr/libvtlscsi.so:
	$(MAKE) -C ../usr libvtlscsi.so
test_compression_select: test_compression_select.c ../include/vtlcart.h $(COMPRESS_SRC) ../usr/libvtlscsi.so
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_compression_select.c $(COMPRESS_SRC) \
		-L ../usr -lvtlscsi -llz4 -lzstd -Wl,-rpath,'$$ORIGIN/../usr'

# open_fifo() from libvtlscsi: never follow a symlink or write a non-FIFO
test_open_fifo: test_open_fifo.c acutest.h ../usr/libvtlscsi.so
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_open_fifo.c \
		-L ../usr -lvtlscsi -Wl,-rpath,'$$ORIGIN/../usr'

# vtlcmd parsing test — compiles vtlcmd.c to .o with main renamed, then links
VTLCMD_SRC = ../usr/cmd/vtlcmd.c
VTLCMD_DEFS = -DMHVTL_VERSION=\"test\" -DMHVTL_GITHASH=\"test\" -DMHVTL_GITDATE=\"test\" \
	-DMHVTL_HOME_PATH=\"/tmp\" -DMHVTL_CONFIG_PATH=\"/tmp\" \
	-DSYSTEMD_SERVICE_DIR=\"/tmp\"
vtlcmd_testobj.o: $(VTLCMD_SRC)
	$(CC) $(CFLAGS) $(IFLAGS) $(VTLCMD_DEFS) -c -o $@ $<
	objcopy --redefine-sym main=vtlcmd_main $@
test_vtlcmd_parsing: test_vtlcmd_parsing.c acutest.h vtlcmd_testobj.o
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_vtlcmd_parsing.c vtlcmd_testobj.o

# find_media_home_directory() from libvtlscsi: per-library home lookup
test_media_home: test_media_home.c acutest.h ../usr/libvtlscsi.so
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_media_home.c \
		-L ../usr -lvtlscsi -Wl,-rpath,'$$ORIGIN/../usr'

# dump_tape / preload_tape option parsing against an empty cartridge
test_dump_tape_options: test_dump_tape_options.c acutest.h helpers/device_conf_gen.h ../usr/libvtlscsi.so
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_dump_tape_options.c \
		-L ../usr -lvtlscsi -Wl,-rpath,'$$ORIGIN/../usr'

# raw_read_mam() from libvtlscsi plus get_cart_type() (vtllibrary helper)
test_raw_read_mam: test_raw_read_mam.c acutest.h ../usr/vtl_cart_type.c ../usr/libvtlscsi.so
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_raw_read_mam.c ../usr/vtl_cart_type.c \
		-L ../usr -lvtlscsi -Wl,-rpath,'$$ORIGIN/../usr'

# A failed index write in write_tape_block() or write_filemarks() rolls back
# to the prior index
test_indx_rollback: test_indx_rollback.c acutest.h ../usr/libvtlscsi.so
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_indx_rollback.c \
		-L ../usr -lvtlscsi -Wl,-rpath,'$$ORIGIN/../usr'

test_dump_tape_output: test_dump_tape_output.c acutest.h ../usr/libvtlscsi.so
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_dump_tape_output.c \
		-L ../usr -lvtlscsi -Wl,-rpath,'$$ORIGIN/../usr'

# dump_tape on a real cartridge: must terminate and print each block once
test_dump_tape_loop: test_dump_tape_loop.c acutest.h helpers/device_conf_gen.h ../usr/libvtlscsi.so
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_dump_tape_loop.c \
		-L ../usr -lvtlscsi -Wl,-rpath,'$$ORIGIN/../usr'

# M8 media MAM from set_media_params(), mktape and edit_tape (libvtlscsi,
# usr/bin/mktape and usr/bin/edit_tape)
test_m8_mam: test_m8_mam.c acutest.h ../usr/libvtlscsi.so
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_m8_mam.c \
		-L ../usr -lvtlscsi -Wl,-rpath,'$$ORIGIN/../usr'

test_q_reply: test_q_reply.c acutest.h ../usr/utils/q.c ../include/utils/q.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_q_reply.c ../usr/utils/q.c -lpthread

test_q_send: test_q_send.c acutest.h ../usr/utils/q.c ../include/utils/q.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ test_q_send.c ../usr/utils/q.c

test_userland_transport: test_userland_transport.c acutest.h helpers/scsi_harness.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_drive_types: test_drive_types.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_tape_io: test_tape_io.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_smc_dvcid: test_smc_dvcid.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_queue_backoff: test_queue_backoff.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_ssc_fixed_ew: test_ssc_fixed_ew.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_conf_sscanf: test_conf_sscanf.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_encryption_mode_page: test_encryption_mode_page.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_smc_load_failure: test_smc_load_failure.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

# MOVE MEDIUM with a drive whose daemon is not running
test_smc_drive_absent: test_smc_drive_absent.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_mode_sense_saved: test_mode_sense_saved.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<
  
test_ssc_load_unload: test_ssc_load_unload.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_ssc_erase: test_ssc_erase.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<
  
test_read_sili: test_read_sili.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<
  
test_ssc_space: test_ssc_space.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_log_volstat_len: test_log_volstat_len.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_ssc_eom: test_ssc_eom.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_encryption_state: test_encryption_state.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_ssc_worm: test_ssc_worm.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

test_lto_m8: test_lto_m8.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

# Links libvtlscsi to set a cartridge's MAM density the way edit_tape does
test_log_volstat_strings: test_log_volstat_strings.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h ../usr/libvtlscsi.so
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $< \
		-L ../usr -lvtlscsi -Wl,-rpath,'$$ORIGIN/../usr'
    
test_encryption_read_key: test_encryption_read_key.c acutest.h helpers/scsi_harness.h helpers/device_conf_gen.h
	$(CC) $(CFLAGS) $(IFLAGS) -o $@ $<

integration: $(INTEGRATION_TESTS)
	@echo "=== Running integration tests (userland transport) ==="
	@for t in $(INTEGRATION_TESTS); do \
		echo "--- $$t ---"; \
		./$$t; \
	done

test: $(TESTS)
	@echo "=== Running unit tests ==="
	@failed=0; \
	for t in $(TESTS); do \
		echo "--- $$t ---"; \
		./$$t; \
		if [ $$? -ne 0 ]; then failed=$$((failed + 1)); fi; \
	done; \
	for t in $(SCRIPT_TESTS); do \
		echo "--- $$t ---"; \
		bash ./$$t; \
		if [ $$? -ne 0 ]; then failed=$$((failed + 1)); fi; \
	done; \
	echo ""; \
	if [ $$failed -eq 0 ]; then \
		echo "=== All test suites passed ==="; \
	else \
		echo "=== $$failed test suite(s) FAILED ==="; \
		exit 1; \
	fi

clean:
	rm -f $(TESTS) $(INTEGRATION_TESTS) bench_compression vtlcmd_testobj.o
