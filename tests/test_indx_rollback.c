/*
 * A failed index write in write_tape_block() must roll the cartridge back to
 * exactly the state before that call: the index keeps every earlier entry
 * and EOD stays at the last good block.  It used to truncate the index to
 * (blk_number - 1) entries, dropping the last good block's entry as well.
 *
 * The failure is injected by wrapping the file backend's pwrite() so one
 * chosen index write fails; everything else is the real file backend.
 */
#include "acutest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ftw.h>
#include <unistd.h>
#include <sys/stat.h>

#include "mhvtl_scsi.h"
#include "vtllib.h"
#include "vtlcart.h"
#include "cart_backend.h"

/* libvtlscsi expects each executable to supply this symbol. */
char mhvtl_driver_name[] = "test_indx_rollback";

#define BLK	512
#define PCL	"INDX01"

static struct cart_backend *orig_be;
static struct cart_backend real_be;
static struct cart_backend fault_be;
static int		   fail_indx_skip = -1;	/* index writes to pass first; -1 = off */
static char		   dir[256];

static ssize_t fault_pwrite(enum cart_stream s, int partition, const void *buf,
			    size_t len, off_t off)
{
	if (s == CART_STREAM_INDX && fail_indx_skip >= 0 && fail_indx_skip-- == 0) {
		errno = ENOSPC;
		return -1;
	}
	return real_be.pwrite(s, partition, buf, len, off);
}

/* Fail the index write that comes after skip successful ones, once. */
static void inject_indx_fault(int skip)
{
	real_be		= *cart_be;
	fault_be	= real_be;
	fault_be.pwrite = fault_pwrite;
	orig_be		= cart_be;
	cart_be		= &fault_be;
	fail_indx_skip	= skip;
}

static void clear_indx_fault(void)
{
	cart_be		= orig_be;
	fail_indx_skip	= -1;
}

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

	snprintf(dir, sizeof(dir), "%s/mhvtl-indx-XXXXXX", tmp && *tmp ? tmp : "/tmp");
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

static off_t indx_size(void)
{
	char	    path[512];
	struct stat st;

	snprintf(path, sizeof(path), "%s/" PCL "/indx.0", dir);
	TEST_ASSERT(stat(path, &st) == 0);
	return st.st_size;
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

static void expect_medium_error_write(const uint8_t *ss)
{
	TEST_CHECK(*ss == SAM_STAT_CHECK_CONDITION);
	TEST_CHECK((sense[2] & 0x0f) == MEDIUM_ERROR);
	TEST_CHECK(sense[12] == 0x0c && sense[13] == 0x00);	/* E_WRITE_ERROR */
}

/* Rewind, read blocks 0..n-1, then expect EOD (no further block). */
static void expect_blocks(int n, uint8_t *ss)
{
	uint8_t buf[BLK];
	int	i;

	*ss = SAM_STAT_GOOD;
	TEST_CHECK(rewind_tape(ss) == 1);
	for (i = 0; i < n; i++)
		expect_read('a' + i, ss);
	*ss = SAM_STAT_GOOD;
	TEST_CHECK(read_tape_block(buf, BLK, ss) != BLK);
}

static void check_failed_block_write(int good_blocks)
{
	uint8_t ss = SAM_STAT_GOOD;
	off_t	unit = 0;
	int	i;

	new_tape(&ss);
	for (i = 0; i < good_blocks; i++) {
		TEST_ASSERT(write_blk('a' + i, &ss) == 0);
		if (i == 0)
			unit = indx_size();
	}
	TEST_CHECK(current_tape_block() == (uint64_t)good_blocks);

	inject_indx_fault(0);
	ss = SAM_STAT_GOOD;
	TEST_CHECK(write_blk('z', &ss) != 0);
	clear_indx_fault();
	expect_medium_error_write(&ss);

	TEST_CHECK(current_tape_block() == (uint64_t)good_blocks);
	if (good_blocks)
		TEST_CHECK(indx_size() == good_blocks * unit);
	else
		TEST_CHECK(indx_size() == 0);

	/* Same session: every earlier block still reads back. */
	expect_blocks(good_blocks, &ss);

	/* After a reload too, and the tape still takes a write at EOD. */
	unload_tape(&ss);
	ss = SAM_STAT_GOOD;
	TEST_ASSERT(load_tape(PCL, &ss) == 0);
	expect_blocks(good_blocks, &ss);
	TEST_CHECK(write_blk('a' + good_blocks, &ss) == 0);
	expect_blocks(good_blocks + 1, &ss);

	drop_tape(&ss);
}

static void test_failed_index_write_keeps_prior_entries(void)
{
	check_failed_block_write(3);
}

static void test_failed_index_write_on_first_block(void)
{
	check_failed_block_write(0);
}

TEST_LIST = {
	{"failed_index_write_keeps_prior_entries", test_failed_index_write_keeps_prior_entries},
	{"failed_index_write_on_first_block", test_failed_index_write_on_first_block},
	{NULL, NULL}};
