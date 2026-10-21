/*
 * Unit tests for raw_read_mam() (usr/vtlcart_raw.c), which vtllibrary's
 * get_cart_type() uses to learn the media type of a raw cartridge: a raw
 * cartridge has no <home>/<barcode>/mam file, its MAM lives in the device's
 * MAM region.
 *
 * raw_read_mam() only needs pread(), so a regular file carrying a superblock
 * and a MAM region stands in for the block device and no root is needed.
 * get_cart_type() itself picks the raw path with S_ISBLK(), which a regular
 * file cannot satisfy; that branch is covered by tests/test_raw_loopback.sh
 * (via tests/get_cart_type_blk.c), which needs loop devices. Here get_cart_type()
 * is checked on a file-backed cartridge to show that path is unchanged.
 */
#include "acutest.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "vtllib.h"
#include "vtlcart.h"
#include "smc.h"
#include "mhvtl_raw.h"
#include "cart_backend_raw.h"
#include "ccan/crc32c/crc32c.h"

/* libvtlscsi expects each executable to supply this symbol. */
char mhvtl_driver_name[] = "test_raw_read_mam";

static char dir[64];

static void mkdir_tmp(void)
{
	snprintf(dir, sizeof(dir), "/tmp/mhvtl-rawmam-XXXXXX");
	TEST_ASSERT(mkdtemp(dir) != NULL);
}

/* Serialize the global mam as the raw backend does:
 * [u32 mam_len][u32 mhvtl_len][mam bytes][mhvtl bytes] */
static uint64_t serialize_mam(uint8_t *out, size_t cap)
{
	int mfd = memfd_create("t-mam", 0);
	int hfd = memfd_create("t-mhvtl", 0);
	uint32_t hdr[2];
	off_t mlen, hlen;

	TEST_ASSERT(mfd >= 0 && hfd >= 0);
	TEST_ASSERT(write_mam(mfd, hfd) == 0);
	mlen = lseek(mfd, 0, SEEK_END);
	hlen = lseek(hfd, 0, SEEK_END);
	TEST_ASSERT(sizeof(hdr) + mlen + hlen <= cap);
	hdr[0] = (uint32_t)mlen;
	hdr[1] = (uint32_t)hlen;
	memcpy(out, hdr, sizeof(hdr));
	TEST_ASSERT(pread(mfd, out + sizeof(hdr), mlen, 0) == mlen);
	TEST_ASSERT(pread(hfd, out + sizeof(hdr) + mlen, hlen, 0) == hlen);
	close(mfd);
	close(hfd);
	return sizeof(hdr) + mlen + hlen;
}

/* Write one superblock at off whose MAM, of media_type, sits at mam_off
 * (no_mam: mam_len 0). bad_crc breaks the superblock checksum. */
static void put_sb(int fd, off_t off, uint8_t media_type, int no_mam,
		   uint64_t mam_off, uint64_t gen, int bad_crc)
{
	static uint8_t mambuf[4096];
	struct mhvtl_raw_superblock sbk;
	uint64_t mam_len;

	init_mam(&mam);
	mam.MediumType = media_type;
	mam.tape_fmt_version = TAPE_FMT_VERSION;
	mam.mam_fmt_version = MAM_VERSION;
	mam_len = serialize_mam(mambuf, sizeof(mambuf));

	memset(&sbk, 0, sizeof(sbk));
	sbk.magic = MVTL_MAGIC;
	sbk.header_len = MVTL_SB_CRC_LEN;
	sbk.version = MVTL_VERSION;
	sbk.generation_id = gen;
	sbk.num_partitions = 1;
	sbk.mam_off = mam_off;
	sbk.mam_len = no_mam ? 0 : mam_len;
	strcpy(sbk.barcode, "RAW001L8");
	sbk.header_crc32c = crc32c(0, &sbk, MVTL_SB_CRC_LEN) ^ (bad_crc ? 1 : 0);

	TEST_ASSERT(pwrite(fd, &sbk, sizeof(sbk), off) == (ssize_t)sizeof(sbk));
	TEST_ASSERT(pwrite(fd, mambuf, mam_len, mam_off) == (ssize_t)mam_len);
}

#define MIRROR_MAM_OFF	(MVTL_MAM_OFF + 65536)

/* Build a raw cartridge image. The primary carries ptype/pgen (bad_primary
 * breaks its checksum); the mirror carries mtype/mgen with its own MAM. */
static void make_image(const char *path, uint8_t ptype, int no_mam,
		       int bad_primary, uint64_t pgen,
		       uint8_t mtype, uint64_t mgen)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);

	TEST_ASSERT(fd >= 0);
	TEST_ASSERT(ftruncate(fd, MVTL_DATA_START) == 0);
	put_sb(fd, MVTL_SB_PRIMARY_OFF, ptype, no_mam, MVTL_MAM_OFF, pgen, bad_primary);
	put_sb(fd, MVTL_SB_MIRROR_OFF, mtype, no_mam, MIRROR_MAM_OFF, mgen, 0);
	close(fd);
}

/* Image built by make_image() must read back as want_type */
static void image_reads(uint8_t want_type, uint8_t ptype, int no_mam,
			int bad_primary, uint64_t pgen,
			uint8_t mtype, uint64_t mgen)
{
	char path[128];
	struct MAM got;

	mkdir_tmp();
	snprintf(path, sizeof(path), "%s/img", dir);
	make_image(path, ptype, no_mam, bad_primary, pgen, mtype, mgen);

	memset(&got, 0, sizeof(got));	/* as get_cart_type() does */
	init_mam(&got);
	TEST_CHECK(raw_read_mam(path, &got) == 0);
	TEST_CHECK(got.MediumType == want_type);
	TEST_MSG("MediumType %d, want %d", got.MediumType, want_type);

	unlink(path);
	rmdir(dir);
}

static void test_reads_media_type(void)
{
	image_reads(MEDIA_TYPE_DATA, MEDIA_TYPE_DATA, 0, 0, 1, MEDIA_TYPE_DATA, 1);
	image_reads(MEDIA_TYPE_WORM, MEDIA_TYPE_WORM, 0, 0, 1, MEDIA_TYPE_WORM, 1);
	image_reads(MEDIA_TYPE_CLEAN, MEDIA_TYPE_CLEAN, 0, 0, 1, MEDIA_TYPE_CLEAN, 1);
}

/* Corrupt primary: the valid mirror is used */
static void test_falls_back_to_mirror(void)
{
	image_reads(MEDIA_TYPE_WORM, MEDIA_TYPE_DATA, 0, 1, 5, MEDIA_TYPE_WORM, 5);
}

/* Both valid: the higher generation_id wins, whichever copy it is in */
static void test_newest_generation_wins(void)
{
	image_reads(MEDIA_TYPE_WORM, MEDIA_TYPE_DATA, 0, 0, 1, MEDIA_TYPE_WORM, 2);
	image_reads(MEDIA_TYPE_DATA, MEDIA_TYPE_DATA, 0, 0, 3, MEDIA_TYPE_WORM, 2);
}

/* No MAM written yet (mam_len 0): raw_read_mam() leaves MediumType alone, so
 * the zeroed struct get_cart_type() passes in reads as data (type 1) */
static void test_no_mam_reads_as_data(void)
{
	image_reads(MEDIA_TYPE_DATA, MEDIA_TYPE_WORM, 1, 0, 1, MEDIA_TYPE_WORM, 1);
}

static void test_rejects_unformatted(void)
{
	char path[128];
	struct MAM got;
	int fd;

	mkdir_tmp();
	snprintf(path, sizeof(path), "%s/img", dir);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	TEST_ASSERT(fd >= 0);
	TEST_ASSERT(ftruncate(fd, MVTL_DATA_START) == 0);
	close(fd);

	init_mam(&got);
	TEST_CHECK(raw_read_mam(path, &got) < 0);
	TEST_CHECK(raw_read_mam("/nonexistent/dev", &got) < 0);

	unlink(path);
	rmdir(dir);
}

/* A file-backed cartridge is still typed from <home>/<barcode>/mam */
static void test_file_backed_unchanged(void)
{
	char path[160], dpath[160];
	int fd, hfd;

	mkdir_tmp();
	snprintf(path, sizeof(path), "%s/FIL001L8", dir);
	TEST_ASSERT(mkdir(path, 0700) == 0);
	snprintf(path, sizeof(path), "%s/FIL001L8/mam", dir);
	snprintf(dpath, sizeof(dpath), "%s/FIL001L8/mhvtl_data", dir);

	init_mam(&mam);
	mam.MediumType = MEDIA_TYPE_WORM;
	mam.tape_fmt_version = TAPE_FMT_VERSION;
	mam.mam_fmt_version = MAM_VERSION;
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	hfd = open(dpath, O_RDWR | O_CREAT | O_TRUNC, 0600);
	TEST_ASSERT(fd >= 0 && hfd >= 0);
	TEST_ASSERT(write_mam(fd, hfd) == 0);
	close(fd);
	close(hfd);

	snprintf(home_directory, sizeof(home_directory), "%s", dir);
	TEST_CHECK(get_cart_type("FIL001L8") == 4);

	unlink(path);
	unlink(dpath);
	snprintf(path, sizeof(path), "%s/FIL001L8", dir);
	rmdir(path);
	rmdir(dir);
}

TEST_LIST = {
	{ "reads_media_type", test_reads_media_type },
	{ "falls_back_to_mirror", test_falls_back_to_mirror },
	{ "newest_generation_wins", test_newest_generation_wins },
	{ "no_mam_reads_as_data", test_no_mam_reads_as_data },
	{ "rejects_unformatted", test_rejects_unformatted },
	{ "file_backed_unchanged", test_file_backed_unchanged },
	{ NULL, NULL }
};
