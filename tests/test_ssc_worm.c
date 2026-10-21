/*
 * Integration tests for WORM and write-protect rules on commands that
 * destroy data: WRITE FILEMARKS (10h), SET CAPACITY (0Bh) and
 * FORMAT MEDIUM (04h).
 *
 * IBM LTO SCSI Reference GA32-0928-07:
 * - 4.14.1 Conditions for Writing (p.54): on WORM, writing is allowed on an
 *   uninitialized cartridge, at BOP when only filemarks lie between BOP and
 *   EOD, and at EOD. A WRITE FILEMARKS after REWIND on a written WORM
 *   cartridge is none of these.
 * - Annex B.8 (p.483): DATA PROTECT, WORM MEDIUM - OVERWRITE ATTEMPTED is
 *   7/30/0C; WRITE PROTECTED is 7/27/00.
 * - 4.0.15 WRITE FILEMARKS (p.181): a count of zero with nothing buffered
 *   always returns GOOD, even on WORM at a position that is not writable.
 * - Table 31 (p.78): SET CAPACITY is a write-type command (WRP column), and
 *   4.2.2 (p.25) has write-type commands on WORM follow the WORM rules.
 * - 5.2.3 FORMAT MEDIUM (p.82): not allowed on a WORM cartridge, 7/30/0C.
 *
 * Runs vtltape with MHVTL_BACKEND=userland against a generated LTO-8
 * configuration; no kernel module or root needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"

#define BLK_SZ 512

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

static const struct drive_type *lto8(void)
{
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++)
		if (strcmp(dt->name, "LTO-8") == 0)
			return dt;
	return NULL;
}

/* Replace the generated data cartridge with a WORM one, same barcode */
static int make_worm(const struct drive_type *dt)
{
	char mktape_path[256];
	char cmd[2048];

	if (harness_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "rm -rf %s/%s && %s%s -C %s -H %s -l %d -m %s -s 500 -t WORM -d %s >/dev/null 2>&1",
		 test_env.home_dir, dt->media_barcode,
		 gen_local_lib_prefix(mktape_path), mktape_path,
		 test_env.conf_dir, test_env.home_dir,
		 TEST_MINOR_LIBRARY, dt->media_barcode, dt->density);
	return system(cmd) == 0 ? 0 : -1;
}

static int set_write_protect(const struct drive_type *dt)
{
	char edit_path[256];
	char cmd[1024];

	if (harness_find_binary("edit_tape", edit_path, sizeof(edit_path)) < 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "%sMHVTL_CONFIG_PATH=%s MHVTL_HOME_PATH=%s %s -l %d -m %s -w on >/dev/null 2>&1",
		 gen_local_lib_prefix(edit_path),
		 test_env.conf_dir, test_env.home_dir, edit_path,
		 TEST_MINOR_LIBRARY, dt->media_barcode);
	return system(cmd) == 0 ? 0 : -1;
}

static int load(void)
{
	const struct drive_type *dt = lto8();
	struct scsi_result r;
	uint8_t tur[6] = { 0x00 };

	if (harness_load_tape(TEST_MINOR_TAPE, dt->media_barcode) < 0)
		return -1;
	for (int i = 0; i < 100; i++) {
		harness_cdb(&daemon_ctx, tur, 6, &r);
		if (r.sam_stat == SAM_STAT_GOOD)
			return 0;
		usleep(100000);
	}
	return -1;
}

static int start_drive(int worm)
{
	const struct drive_type *dt = lto8();

	if (!dt || gen_test_env(&test_env, dt) < 0)
		return -1;
	if (worm && make_worm(dt) < 0)
		return -1;
	if (harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env) < 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	return load();
}

static void stop_drive(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

static int set_capacity(struct scsi_result *r)
{
	uint8_t cdb[6] = { 0x0b, 0, 0, 0xff, 0xff, 0 };

	return harness_cdb(&daemon_ctx, cdb, 6, r);
}

static int format_medium(struct scsi_result *r)
{
	uint8_t cdb[6] = { 0x04, 0, 0, 0, 0, 0 };

	return harness_cdb(&daemon_ctx, cdb, 6, r);
}

static int sense_is(const struct scsi_result *r, uint8_t key, uint8_t asc,
		    uint8_t ascq)
{
	return r->sam_stat == SAM_STAT_CHECK_CONDITION &&
	       (r->sense[2] & 0x0f) == key &&
	       r->sense[12] == asc && r->sense[13] == ascq;
}

/* Three blocks: more than the 1-2 record label WORM lets be overwritten */
static void write_blocks(uint8_t b[3][BLK_SZ])
{
	struct scsi_result r;

	for (int i = 0; i < 3; i++) {
		memset(b[i], 0x11 * (i + 1), BLK_SZ);
		harness_tape_write(&daemon_ctx, b[i], BLK_SZ, &r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "write block %d", i);
	}
}

static void check_blocks(uint8_t b[3][BLK_SZ], const char *what)
{
	uint8_t rb[BLK_SZ];
	struct scsi_result r;

	harness_tape_rewind(&daemon_ctx, &r);
	for (int i = 0; i < 3; i++) {
		harness_tape_read(&daemon_ctx, rb, BLK_SZ, &r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD &&
			    memcmp(rb, b[i], BLK_SZ) == 0,
			    "%s: block %d stat=%d sense %x/%02x/%02x",
			    what, i, r.sam_stat, r.sense[2] & 0x0f,
			    r.sense[12], r.sense[13]);
	}
}

/* Written WORM, REWIND, WRITE FILEMARKS 1: refused, data intact */
void test_worm_wfm_at_bot_refused(void)
{
	uint8_t b[3][BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive(1) == 0);
	write_blocks(b);
	harness_tape_rewind(&daemon_ctx, &r);

	harness_tape_write_fm(&daemon_ctx, 1, &r);
	TEST_CHECK_(sense_is(&r, 0x07, 0x30, 0x0c),
		    "WRITE FILEMARKS 1 at BOT stat=%d sense %x/%02x/%02x (expected 7/30/0C)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	check_blocks(b, "after refused WRITE FILEMARKS");

	/* Still intact once reloaded */
	harness_unload_tape(TEST_MINOR_TAPE);
	TEST_ASSERT(load() == 0);
	check_blocks(b, "after reload");

	stop_drive();
}

/* WRITE FILEMARKS 0 is a flush: GOOD on WORM even when not writable */
void test_worm_wfm_zero_good(void)
{
	uint8_t b[3][BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive(1) == 0);
	write_blocks(b);
	harness_tape_rewind(&daemon_ctx, &r);

	harness_tape_write_fm(&daemon_ctx, 0, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
		    "WRITE FILEMARKS 0 at BOT stat=%d sense %x/%02x/%02x",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	check_blocks(b, "after WRITE FILEMARKS 0");

	stop_drive();
}

/* Empty WORM: filemarks may be written, and appended after data */
void test_worm_wfm_empty_and_append(void)
{
	uint8_t b[3][BLK_SZ], rb[BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive(1) == 0);

	harness_tape_write_fm(&daemon_ctx, 1, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
		    "WRITE FILEMARKS 1 on empty WORM stat=%d sense %x/%02x/%02x",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	write_blocks(b);
	harness_tape_write_fm(&daemon_ctx, 1, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "WRITE FILEMARKS 1 at EOD");

	harness_tape_rewind(&daemon_ctx, &r);
	harness_tape_read(&daemon_ctx, rb, BLK_SZ, &r);
	TEST_CHECK_(sense_is(&r, 0x00, 0x00, 0x01),
		    "filemark at block 0: sense %x/%02x/%02x",
		    r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	harness_tape_read(&daemon_ctx, rb, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && memcmp(rb, b[0], BLK_SZ) == 0,
		    "block 1 readable");

	stop_drive();
}

/* Only a filemark between BOP and EOD: it may be overwritten from BOP */
void test_worm_overwrite_lone_filemark(void)
{
	uint8_t b[BLK_SZ], rb[BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive(1) == 0);

	harness_tape_write_fm(&daemon_ctx, 1, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "WRITE FILEMARKS 1");

	/* Reload so the writable state comes from the medium, not the session */
	harness_unload_tape(TEST_MINOR_TAPE);
	TEST_ASSERT(load() == 0);

	harness_tape_write_fm(&daemon_ctx, 1, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
		    "WRITE FILEMARKS 1 over lone filemark stat=%d sense %x/%02x/%02x",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	harness_tape_rewind(&daemon_ctx, &r);
	memset(b, 0x77, BLK_SZ);
	harness_tape_write(&daemon_ctx, b, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
		    "WRITE over lone filemark stat=%d sense %x/%02x/%02x",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	harness_tape_rewind(&daemon_ctx, &r);
	harness_tape_read(&daemon_ctx, rb, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && memcmp(rb, b, BLK_SZ) == 0,
		    "block 0 holds the new data");

	stop_drive();
}

void test_worm_set_capacity_refused(void)
{
	uint8_t b[3][BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive(1) == 0);
	write_blocks(b);
	harness_tape_rewind(&daemon_ctx, &r);

	set_capacity(&r);
	TEST_CHECK_(sense_is(&r, 0x07, 0x30, 0x0c),
		    "SET CAPACITY on written WORM stat=%d sense %x/%02x/%02x (expected 7/30/0C)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	check_blocks(b, "after refused SET CAPACITY");

	stop_drive();
}

void test_worm_format_medium_refused(void)
{
	struct scsi_result r;

	/* Not even on an empty WORM cartridge */
	TEST_ASSERT(start_drive(1) == 0);

	format_medium(&r);
	TEST_CHECK_(sense_is(&r, 0x07, 0x30, 0x0c),
		    "FORMAT MEDIUM on WORM stat=%d sense %x/%02x/%02x (expected 7/30/0C)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	stop_drive();
}

/* 5.2.3 refuses WORM before write protect: 7/30/0C, not 7/27/00 */
void test_worm_format_medium_write_protected(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_drive(1) == 0);
	harness_unload_tape(TEST_MINOR_TAPE);
	TEST_ASSERT(set_write_protect(lto8()) == 0);
	TEST_ASSERT(load() == 0);

	format_medium(&r);
	TEST_CHECK_(sense_is(&r, 0x07, 0x30, 0x0c),
		    "FORMAT MEDIUM on write-protected WORM stat=%d sense %x/%02x/%02x (expected 7/30/0C)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	stop_drive();
}

/* NOT READY, MEDIUM NOT PRESENT (GA32-0928-07 4.0.6, p.169) */
void test_set_capacity_no_media(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_drive(0) == 0);
	harness_unload_tape(TEST_MINOR_TAPE);

	set_capacity(&r);
	TEST_CHECK_(sense_is(&r, 0x02, 0x3a, 0x00),
		    "SET CAPACITY with no media stat=%d sense %x/%02x/%02x (expected 2/3A/00)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	stop_drive();
}

/* ILLEGAL REQUEST, POSITION PAST BEGINNING OF MEDIUM (4.0.6, p.169) */
void test_set_capacity_not_at_bop(void)
{
	uint8_t b[3][BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive(0) == 0);
	write_blocks(b);

	set_capacity(&r);
	TEST_CHECK_(sense_is(&r, 0x05, 0x3b, 0x0c),
		    "SET CAPACITY not at BOP stat=%d sense %x/%02x/%02x (expected 5/3B/0C)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	check_blocks(b, "after refused SET CAPACITY");

	stop_drive();
}

void test_set_capacity_write_protected(void)
{
	uint8_t b[3][BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive(0) == 0);
	write_blocks(b);
	harness_unload_tape(TEST_MINOR_TAPE);
	TEST_ASSERT(set_write_protect(lto8()) == 0);
	TEST_ASSERT(load() == 0);

	set_capacity(&r);
	TEST_CHECK_(sense_is(&r, 0x07, 0x27, 0x00),
		    "SET CAPACITY on write-protected media stat=%d sense %x/%02x/%02x (expected 7/27/00)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	check_blocks(b, "after refused SET CAPACITY");

	stop_drive();
}

void test_set_capacity_data(void)
{
	uint8_t b[3][BLK_SZ], rb[BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive(0) == 0);
	write_blocks(b);
	harness_tape_rewind(&daemon_ctx, &r);

	set_capacity(&r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
		    "SET CAPACITY on data cartridge stat=%d sense %x/%02x/%02x",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	/* All data is gone: BLANK CHECK, END-OF-DATA DETECTED (8/00/05) */
	harness_tape_rewind(&daemon_ctx, &r);
	harness_tape_read(&daemon_ctx, rb, BLK_SZ, &r);
	TEST_CHECK_(sense_is(&r, 0x08, 0x00, 0x05),
		    "READ after SET CAPACITY stat=%d sense %x/%02x/%02x (expected 8/00/05)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	stop_drive();
}

TEST_LIST = {
	{ "worm_wfm_at_bot_refused",       test_worm_wfm_at_bot_refused },
	{ "worm_wfm_zero_good",            test_worm_wfm_zero_good },
	{ "worm_wfm_empty_and_append",     test_worm_wfm_empty_and_append },
	{ "worm_overwrite_lone_filemark",  test_worm_overwrite_lone_filemark },
	{ "worm_set_capacity_refused",     test_worm_set_capacity_refused },
	{ "worm_format_medium_refused",    test_worm_format_medium_refused },
	{ "worm_format_medium_write_protected", test_worm_format_medium_write_protected },
	{ "set_capacity_no_media",         test_set_capacity_no_media },
	{ "set_capacity_not_at_bop",       test_set_capacity_not_at_bop },
	{ "set_capacity_write_protected",  test_set_capacity_write_protected },
	{ "set_capacity_data",             test_set_capacity_data },
	{ NULL, NULL }
};
