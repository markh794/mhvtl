/*
 * test_raw_backend — direct round-trip of the raw block-device backend.
 *
 * Driven by tests/test_raw_loopback.sh, which formats a loopback device and
 * passes its path as argv[1]. Exercises cart_backend_raw directly (raw_open +
 * the seam ops) plus direct on-disk superblock validation. Must run as root
 * (the device is already formatted by the harness).
 *
 * Checks: superblock validates on open; DATA/INDX/META writes read back
 * byte-identical; a pwrite past a region cap returns ENOSPC; truncate records a
 * logical used length in the superblock without resizing; sync bumps the
 * generation and keeps both superblocks CRC-valid.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "vtllib.h"		/* MAX_PARTITIONS */
#include "cart_backend_raw.h"
#include "mhvtl_raw.h"

char mhvtl_driver_name[] = "test_raw_backend";

/* crc32c lives in libvtlscsi (usr/utils/crc32c.c). */
extern uint32_t crc32c(uint32_t crc, const void *data, unsigned int length);

static int fails;
#define CHECK(cond, msg) do { \
	if (cond) { printf("  ok   : %s\n", msg); } \
	else { printf("  FAIL : %s (errno=%s)\n", msg, strerror(errno)); fails++; } \
} while (0)

/* Read + validate a superblock copy straight off the device. */
static int read_sb(const char *dev, uint64_t off, struct mhvtl_raw_superblock *sb)
{
	int fd = open(dev, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = pread(fd, sb, sizeof(*sb), (off_t)off);
	close(fd);
	if (n != (ssize_t)sizeof(*sb))
		return -1;
	if (sb->magic != MVTL_MAGIC)
		return -1;
	if (crc32c(0, sb, MVTL_SB_CRC_LEN) != sb->header_crc32c)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <block-device>\n", argv[0]);
		return 2;
	}
	const char *dev = argv[1];
	struct mhvtl_raw_superblock sb0, sb1, sbm;

	printf("test_raw_backend on %s\n", dev);

	/* 1. Both superblocks validate off the device after formatting. */
	CHECK(read_sb(dev, MVTL_SB_PRIMARY_OFF, &sb0) == 0, "primary superblock valid");
	CHECK(read_sb(dev, MVTL_SB_MIRROR_OFF, &sbm) == 0, "mirror superblock valid");
	CHECK(sb0.num_partitions >= 1 && sb0.num_partitions <= MVTL_MAX_PARTITIONS,
	      "num_partitions in range");
	uint64_t gen_before = sb0.generation_id;

	/* 2. raw_open loads + validates the superblock into backend state. */
	CHECK(raw_open(dev) == 0, "raw_open succeeds");

	/* 3. byte-identical round-trip on each stream of partition 0. */
	unsigned char wr[8192], rd[8192];
	for (size_t i = 0; i < sizeof(wr); i++)
		wr[i] = (unsigned char)(i * 7 + 3);

	enum cart_stream streams[3] = { CART_STREAM_DATA, CART_STREAM_INDX, CART_STREAM_META };
	const char *names[3] = { "DATA", "INDX", "META" };
	for (int s = 0; s < 3; s++) {
		/* write two records at distinct offsets, read each back */
		ssize_t w1 = cart_backend_raw.pwrite(streams[s], 0, wr, 4096, 0);
		ssize_t w2 = cart_backend_raw.pwrite(streams[s], 0, wr + 4096, 2048, 4096);
		cart_backend_raw.sync(streams[s], 0);
		memset(rd, 0, sizeof(rd));
		ssize_t r1 = cart_backend_raw.pread(streams[s], 0, rd, 4096, 0);
		ssize_t r2 = cart_backend_raw.pread(streams[s], 0, rd + 4096, 2048, 4096);
		char m[64];
		snprintf(m, sizeof(m), "%s round-trip byte-identical", names[s]);
		CHECK(w1 == 4096 && w2 == 2048 && r1 == 4096 && r2 == 2048 &&
		      memcmp(wr, rd, 6144) == 0, m);
	}

	/* 4. pwrite past the region cap returns ENOSPC. */
	errno = 0;
	ssize_t bad = cart_backend_raw.pwrite(CART_STREAM_DATA, 0, wr, sizeof(wr),
					      (off_t)sb0.total_capacity_bytes);
	CHECK(bad < 0 && errno == ENOSPC, "pwrite past cap -> ENOSPC");

	/* 5. truncate records a logical used length without resizing the device. */
	CHECK(cart_backend_raw.truncate(CART_STREAM_DATA, 0, 12345) == 0,
	      "truncate(DATA,0,12345) ok");
	CHECK(read_sb(dev, MVTL_SB_PRIMARY_OFF, &sb1) == 0 &&
	      sb1.part[0].data_used == 12345,
	      "superblock records data_used=12345 (no device resize)");

	/* 6. a sync bumped the generation and kept both superblocks CRC-valid. */
	CHECK(sb1.generation_id > gen_before, "generation_id advanced after sync");
	CHECK(read_sb(dev, MVTL_SB_MIRROR_OFF, &sbm) == 0 &&
	      sbm.generation_id == sb1.generation_id,
	      "mirror superblock matches primary generation");

	raw_close();

	printf("%s: %d check(s) failed\n", fails ? "FAILED" : "SUCCESS", fails);
	return fails ? 1 : 0;
}
