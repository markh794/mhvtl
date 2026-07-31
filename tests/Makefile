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
TESTS = test_byteshift test_crc test_sense test_smc_slots test_vpd test_log_volstat test_vtlcmd_parsing

.PHONY: all test clean

all: $(TESTS)

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

test: $(TESTS)
	@echo "=== Running unit tests ==="
	@failed=0; \
	for t in $(TESTS); do \
		echo "--- $$t ---"; \
		./$$t; \
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
	rm -f $(TESTS) vtlcmd_testobj.o
