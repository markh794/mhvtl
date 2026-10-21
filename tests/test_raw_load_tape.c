/*
 * test_raw_load_tape — end-to-end integration of the raw backend with the
 * tape stack. Driven by tests/test_raw_loopback.sh, which formats a loopback
 * block device with mhvtl-raw-format and passes <device> <barcode>.
 *
 * Unlike test_raw_backend (which pokes cart_backend_raw directly), this drives
 * the real load_tape()/write_tape_block()/read_tape_block()/unload_tape() entry
 * points the vtltape daemon uses, proving the whole increment-4 integration on
 * a real block device:
 *   - a home_directory/<barcode> symlink to the device resolves to S_ISBLK and
 *     cart_select_backend() picks the raw backend,
 *   - load_tape() opens the device, validates the superblock, serves the MAM
 *     from the superblock region, derives the partition count from it, and
 *     reconstructs position from the partition table's *_used (no data.N / fstat),
 *   - a data block round-trips, a larger read returns the record length (the
 *     short-read semantic), and the data survives an unload + reload.
 *
 * Must run as root (the harness formats the device; this opens it O_RDWR).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "mhvtl_scsi.h"		/* SAM_STAT_GOOD */
#include "vtllib.h"		/* home_directory, mam, struct MAM */
#include "be_byteshift.h"
#include "vtlcart.h"		/* load_tape/unload_tape/write/read/rewind */
#include "cart_backend.h"
#include "cart_backend_raw.h"

char mhvtl_driver_name[] = "test_raw_load_tape";

static int fails;
#define CHECK(cond, msg) do { \
	if (cond) { printf("  ok   : %s\n", msg); } \
	else { printf("  FAIL : %s\n", msg); fails++; } \
} while (0)

#define BLK 4096

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <block-device> <barcode>\n", argv[0]);
		return 2;
	}
	const char *dev = argv[1];
	const char *barcode = argv[2];

	char home[] = "/tmp/mhvtl-rawhome-XXXXXX";
	char link[512];
	uint8_t ss = SAM_STAT_GOOD;
	uint8_t wbuf[BLK], rbuf[65536];
	int rc;

	printf("test_raw_load_tape on %s (barcode %s)\n", dev, barcode);

	if (!mkdtemp(home)) {
		perror("mkdtemp");
		return 2;
	}
	strncpy(home_directory, home, HOME_DIR_PATH_SZ);
	home_directory[HOME_DIR_PATH_SZ] = '\0';

	snprintf(link, sizeof(link), "%s/%s", home, barcode);
	if (symlink(dev, link) < 0) {
		perror("symlink");
		return 2;
	}

	for (int i = 0; i < BLK; i++)
		wbuf[i] = (uint8_t)(i * 31 + 7);

	/* 1. Load the raw cartridge through the real tape entry point. */
	rc = load_tape(barcode, &ss);
	CHECK(rc == 0 && ss == SAM_STAT_GOOD, "load_tape() loads the raw cartridge");
	CHECK(cart_be == &cart_backend_raw, "raw backend selected via S_ISBLK");
	CHECK(mam.num_partitions >= 1, "partition count came from the superblock");

	/* The format wrote a MAM: without a media type and a capacity the drive
	 * reports end of medium on the first write. The harness formats an L8
	 * barcode with a 92,8 split. */
	uint64_t cap_sum = 0;
	for (int p = 0; p < mam.num_partitions; p++)
		cap_sum += get_unaligned_be64(&mam.partition_capacity[p]);
	CHECK(mam.MediumType == MEDIA_TYPE_DATA && mam.MediaType == Media_LTO8,
	      "MAM media type taken from the L8 barcode suffix");
	CHECK(get_unaligned_be64(&mam.max_capacity) > 0 &&
	      get_unaligned_be64(&mam.partition_capacity[1]) > 0 &&
	      get_unaligned_be64(&mam.max_capacity) == cap_sum,
	      "MAM capacity is the sum of the recorded partition capacities");

	/* 2. Write a data block at BOT through the tape layer. */
	rewind_tape(&ss);
	rc = write_tape_block(wbuf, BLK, 0, NULL, 0, 0, 0, &ss);
	CHECK(rc == 0 && ss == SAM_STAT_GOOD, "write_tape_block() to raw cartridge");

	/* 3. Read it back; a 64k read of a 4k record returns the record length. */
	rewind_tape(&ss);
	memset(rbuf, 0xde, sizeof(rbuf));
	rc = (int)read_tape_block(rbuf, sizeof(rbuf), &ss);
	CHECK(rc == BLK, "short read returns the record length, not the buffer size");
	CHECK(memcmp(wbuf, rbuf, BLK) == 0, "data read back byte-identical");

	/* 4. The data survives an unload + reload (durable on the device). */
	unload_tape(&ss);
	rc = load_tape(barcode, &ss);
	CHECK(rc == 0 && ss == SAM_STAT_GOOD, "reload after unload");
	rewind_tape(&ss);
	memset(rbuf, 0, sizeof(rbuf));
	rc = (int)read_tape_block(rbuf, BLK, &ss);
	CHECK(rc == BLK && memcmp(wbuf, rbuf, BLK) == 0,
	      "data persists across unload/reload");
	unload_tape(&ss);

	unlink(link);
	rmdir(home);

	printf("%s: %d check(s) failed\n", fails ? "FAILED" : "SUCCESS", fails);
	return fails ? 1 : 0;
}
