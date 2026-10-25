/*
 * change_partition() must be transactional: if it cannot open the requested
 * partition (missing or unreadable files) it must leave the drive on the
 * partition it was already on, with that partition's files still open and
 * c_pos->partition_id unchanged - not half-open on a partition whose files
 * never opened (markh794/mhvtl#331).
 *
 * The failure is produced for real, without fault injection: a one-partition
 * cartridge has no data.1/indx.1/meta.1, so change_partition(1) makes
 * open_partition(1) fail. The old code closed partition 0 and advanced
 * c_pos->partition_id before that open, leaving partition 0 closed and the
 * position pointing at a partition with no open files.
 *
 * Links libvtlscsi.so and drives the real file backend, like
 * tests/test_indx_rollback.c.
 */
#include "acutest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ftw.h>
#include <unistd.h>
#include <sys/stat.h>

#include "mhvtl_scsi.h"
#include "vtllib.h"
#include "vtlcart.h"

/* libvtlscsi expects each executable to supply this symbol. */
char mhvtl_driver_name[] = "test_change_partition";

#define BLK 512
#define PCL "CHPART01"

static char dir[PATH_MAX];

static int rm_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
	(void)sb;
	(void)type;
	(void)ftw;
	return remove(path);
}

static void new_tape(uint8_t *ss)
{
	const char *tmp = getenv("TMPDIR");

	snprintf(dir, sizeof(dir), "%s/mhvtl-chpart-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	TEST_ASSERT(mkdtemp(dir) != NULL);
	strncpy(home_directory, dir, HOME_DIR_PATH_SZ);
	home_directory[HOME_DIR_PATH_SZ] = '\0';

	memset(&mam, 0, sizeof(mam));
	mam.tape_fmt_version = TAPE_FMT_VERSION;
	mam.mam_fmt_version  = MAM_VERSION;
	mam.num_partitions   = 1;
	*ss = SAM_STAT_GOOD;
	TEST_ASSERT(create_tape(PCL, ss) == 0);
	TEST_ASSERT(load_tape(PCL, ss) == 0);
}

static void drop_tape(uint8_t *ss)
{
	unload_tape(ss);
	nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static int write_blk(uint8_t fill, uint8_t *ss)
{
	uint8_t buf[BLK];

	memset(buf, fill, sizeof(buf));
	return write_tape_block(buf, BLK, 0, NULL, 0, 0, 0, ss);
}

static void expect_read(uint8_t fill, uint8_t *ss)
{
	uint8_t buf[BLK], want[BLK];

	memset(want, fill, sizeof(want));
	*ss = SAM_STAT_GOOD;
	TEST_CHECK(read_tape_block(buf, BLK, ss) == BLK);
	TEST_CHECK(memcmp(buf, want, BLK) == 0);
}

void test_failed_change_partition_is_transactional(void)
{
	uint8_t ss = SAM_STAT_GOOD;

	new_tape(&ss);

	/* Two blocks on partition 0, confirm they read back. */
	TEST_ASSERT(write_blk('a', &ss) == 0);
	TEST_ASSERT(write_blk('b', &ss) == 0);
	TEST_CHECK(rewind_tape(&ss) == 1);
	expect_read('a', &ss);
	expect_read('b', &ss);

	TEST_CHECK(c_pos->partition_id == 0);

	/* Partition 1 has no backing files on a one-partition cartridge, so
	 * open_partition(1) fails. The call must report the error and change
	 * nothing. */
	TEST_CHECK(change_partition(1) != 0);

	/* c_pos must still point at partition 0. */
	TEST_CHECK_(c_pos->partition_id == 0,
		    "partition_id after failed change_partition = %u (want 0)",
		    c_pos->partition_id);

	/* Partition 0 must still be open and usable: rewind and read both
	 * blocks. On the pre-fix code partition 0 was closed, so this fails. */
	TEST_CHECK(rewind_tape(&ss) == 1);
	expect_read('a', &ss);
	expect_read('b', &ss);

	drop_tape(&ss);
}

TEST_LIST = {
	{ "failed_change_partition_is_transactional", test_failed_change_partition_is_transactional },
	{ NULL, NULL }
};
