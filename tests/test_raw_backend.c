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

#include "vtllib.h"		/* MAX_PARTITIONS, struct MAM, init_mam, mam */
#include "vtlcart.h"		/* TAPE_FMT_VERSION */
#include "cart_backend_raw.h"
#include "mhvtl_raw.h"
#include "be_byteshift.h"

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

	/* 7. num_partitions reflects the formatted layout. */
	CHECK(cart_backend_raw.num_partitions() == (int)sb0.num_partitions,
	      "num_partitions matches superblock");

	/*
	 * 8. MAM round-trips through the superblock region. Initialise the global
	 * MAM, save it, snapshot the serialized region bytes, reload into a second
	 * MAM (versions must survive), promote the reload to the global MAM and
	 * re-save: a stable serialize->deserialize->serialize leaves the region
	 * byte-identical. Proves the region is live (not the mam_len==0 stub).
	 */
	init_mam(&mam);
	/* init_mam() leaves the format versions at 0; the creating code stamps
	 * them. Set them so the round-trip has recognisable version words. */
	mam.mam_fmt_version = MAM_VERSION;
	mam.tape_fmt_version = TAPE_FMT_VERSION;
	CHECK(cart_backend_raw.save_mam(&mam) == 0, "save_mam succeeds");
	{
		struct mhvtl_raw_superblock sbx;
		struct MAM mam2;
		uint8_t ss = 0;
		uint8_t *a = NULL, *b = NULL;
		int fd = -1, ok = 0;

		if (read_sb(dev, MVTL_SB_PRIMARY_OFF, &sbx) == 0 &&
		    sbx.mam_len > 8 && sbx.mam_len <= MVTL_MAM_RESERVE) {
			uint64_t len = sbx.mam_len;
			struct mhvtl_raw_superblock sby;

			a = malloc(len);
			b = malloc(len);
			fd = open(dev, O_RDONLY);
			if (a && b && fd >= 0 &&
			    pread(fd, a, len, (off_t)sbx.mam_off) == (ssize_t)len) {
				/* Seed the attribute table (ids/lengths) the way
				 * load_tape() does before load_mam(): read_mam()
				 * fills values only for attribute ids already
				 * present in the target MAM. */
				init_mam(&mam2);
				if (cart_backend_raw.load_mam(&mam2, &ss) == 0 &&
				    mam2.mam_fmt_version == MAM_VERSION &&
				    mam2.tape_fmt_version == TAPE_FMT_VERSION) {
					memcpy(&mam, &mam2, sizeof(mam));
					if (cart_backend_raw.save_mam(&mam) == 0 &&
					    read_sb(dev, MVTL_SB_PRIMARY_OFF, &sby) == 0 &&
					    sby.mam_len == len &&
					    pread(fd, b, len, (off_t)sbx.mam_off) ==
						    (ssize_t)len &&
					    memcmp(a, b, len) == 0)
						ok = 1;
				}
			}
		}
		if (fd >= 0)
			close(fd);
		free(a);
		free(b);
		CHECK(ok, "MAM save/load/save round-trip byte-identical");
	}

	/* 8b. An M8 MAM (LTO-7 media in the LTO-8 Type M format, density 5Dh,
	 *     GA32-0928-07 table 113, p.142) survives the superblock region. */
	{
		struct MAM mam2;
		uint8_t ss = 0;

		init_mam(&mam);
		mam.mam_fmt_version = MAM_VERSION;
		mam.tape_fmt_version = TAPE_FMT_VERSION;
		mam.MediumType = MEDIA_TYPE_DATA;
		CHECK(set_media_params(&mam, "LTO8M8") == 0,
		      "set_media_params knows LTO8M8");
		CHECK(cart_backend_raw.save_mam(&mam) == 0, "save_mam of an M8 MAM");
		init_mam(&mam2);
		CHECK(cart_backend_raw.load_mam(&mam2, &ss) == 0 &&
		      mam2.MediaType == Media_LTO8_M8 &&
		      mam2.MediumType == MEDIA_TYPE_DATA &&
		      mam2.MediumDensityCode == medium_density_code_lto8_m8 &&
		      mam2.max_partitions == 4 &&
		      !strncmp(mam2.media_info.density_name, "U-832M", 6) &&
		      get_unaligned_be64(&mam2.max_capacity) == 9000000000000ULL,
		      "M8 media type, density, partitions and capacity reload");
	}

	/* 9. raw_probe reads the cartridge identity off the device without
	 *    disturbing the open cartridge (barcode/uuid/partition count match
	 *    the on-disk superblock; immutable across the writes above). */
	{
		struct mhvtl_raw_probe pr;
		CHECK(raw_probe(dev, &pr) == 0, "raw_probe finds a valid cartridge");
		CHECK(strncmp(pr.barcode, sb0.barcode, sizeof(sb0.barcode)) == 0,
		      "raw_probe barcode matches superblock");
		CHECK(pr.num_partitions == sb0.num_partitions,
		      "raw_probe num_partitions matches superblock");
		CHECK(memcmp(pr.cartridge_uuid, sb0.cartridge_uuid,
			     sizeof(pr.cartridge_uuid)) == 0,
		      "raw_probe cartridge_uuid matches superblock");
	}

	raw_close();

	printf("%s: %d check(s) failed\n", fails ? "FAILED" : "SUCCESS", fails);
	return fails ? 1 : 0;
}
