/*
 * Integration tests for ERASE (19h) on an SSC drive.
 *
 * ERASE writes EOD at the current position, from any position, and leaves
 * the drive positioned at that EOD (IBM LTO SCSI Reference GA32-0928-07,
 * 5.2.2 ERASE, p.81). Erasing write-protected media reports DATA PROTECT,
 * WRITE PROTECTED, 7/27/00 (GA32-0928-07 Annex B.8, p.483).
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

static int start_drive(int write_protect)
{
	const struct drive_type *dt = lto8();
	struct scsi_result r;
	uint8_t tur[6] = { 0x00 };

	if (!dt || gen_test_env(&test_env, dt) < 0)
		return -1;
	if (write_protect && set_write_protect(dt) < 0)
		return -1;
	if (harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env) < 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
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

static void stop_drive(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

static int erase(int long_bit, struct scsi_result *r)
{
	uint8_t cdb[6] = { 0x19, (uint8_t)(long_bit ? 0x01 : 0x00), 0, 0, 0, 0 };

	return harness_cdb(&daemon_ctx, cdb, 6, r);
}

static int space(uint8_t code, uint32_t count, struct scsi_result *r)
{
	uint8_t cdb[6] = { 0x11, code, (count >> 16) & 0xff,
			   (count >> 8) & 0xff, count & 0xff, 0 };

	return harness_cdb(&daemon_ctx, cdb, 6, r);
}

/* READ POSITION, short form: first block location, bytes 4-7 */
static long read_position(void)
{
	uint8_t cdb[10] = { 0x34, 0 };
	uint8_t buf[20];
	struct scsi_result r;

	memset(buf, 0, sizeof(buf));
	if (harness_send_cdb(&daemon_ctx, cdb, 10, &r, buf, sizeof(buf)) < 0 ||
	    r.sam_stat != SAM_STAT_GOOD)
		return -1;
	return ((long)buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
}

static int sense_is(const struct scsi_result *r, uint8_t key, uint8_t asc,
		    uint8_t ascq)
{
	return r->sam_stat == SAM_STAT_CHECK_CONDITION &&
	       (r->sense[2] & 0x0f) == key &&
	       r->sense[12] == asc && r->sense[13] == ascq;
}

static void check_blank(const char *what)
{
	uint8_t rb[BLK_SZ];
	struct scsi_result r;

	harness_tape_read(&daemon_ctx, rb, sizeof(rb), &r);
	TEST_CHECK_(sense_is(&r, 0x08, 0x00, 0x05),
		    "%s: READ at EOD stat=%d sense %x/%02x/%02x (expected 8/00/05)",
		    what, r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
}

/* 3 blocks, rewind, SPACE 1 block, short ERASE: EOD at block 1. */
void test_erase_mid_tape(void)
{
	uint8_t b[3][BLK_SZ], rb[BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive(0) == 0);

	for (int i = 0; i < 3; i++) {
		memset(b[i], 0x11 * (i + 1), BLK_SZ);
		harness_tape_write(&daemon_ctx, b[i], BLK_SZ, &r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "write block %d", i);
	}
	harness_tape_rewind(&daemon_ctx, &r);
	space(0, 1, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "SPACE 1 block");

	erase(0, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
		    "ERASE at block 1 stat=%d sense %x/%02x/%02x",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	TEST_CHECK_(read_position() == 1, "position %ld after ERASE (expected 1)",
		    read_position());
	check_blank("after ERASE");

	/* Block 0 is still there and is followed by EOD */
	harness_tape_rewind(&daemon_ctx, &r);
	harness_tape_read(&daemon_ctx, rb, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && memcmp(rb, b[0], BLK_SZ) == 0,
		    "block 0 survives the ERASE");
	check_blank("block 1 after rewind");

	stop_drive();
}

/* Filemarks before the erase point survive; LONG=1 also truncates there. */
void test_erase_long_keeps_filemarks(void)
{
	uint8_t b[BLK_SZ], rb[BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive(0) == 0);

	memset(b, 0x5a, BLK_SZ);
	harness_tape_write(&daemon_ctx, b, BLK_SZ, &r);
	harness_tape_write_fm(&daemon_ctx, 1, &r);
	harness_tape_write(&daemon_ctx, b, BLK_SZ, &r);
	harness_tape_write(&daemon_ctx, b, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "write data");

	harness_tape_rewind(&daemon_ctx, &r);
	space(1, 1, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "SPACE 1 filemark");
	TEST_CHECK_(read_position() == 2, "position %ld after SPACE (expected 2)",
		    read_position());

	erase(1, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
		    "long ERASE at block 2 stat=%d sense %x/%02x/%02x",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	TEST_CHECK_(read_position() == 2, "position %ld after ERASE (expected 2)",
		    read_position());
	check_blank("after long ERASE");

	harness_tape_rewind(&daemon_ctx, &r);
	harness_tape_read(&daemon_ctx, rb, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "block 0 readable");
	/* FILEMARK detected: NO SENSE with the FILEMARK bit, 0/00/01 */
	harness_tape_read(&daemon_ctx, rb, BLK_SZ, &r);
	TEST_CHECK_(sense_is(&r, 0x00, 0x00, 0x01) && (r.sense[2] & 0x80),
		    "filemark at block 1 survives: sense[2]=0x%02x %02x/%02x",
		    r.sense[2], r.sense[12], r.sense[13]);
	check_blank("block 2 after rewind");

	stop_drive();
}

void test_erase_at_bot(void)
{
	uint8_t b[BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive(0) == 0);

	memset(b, 0xa5, BLK_SZ);
	harness_tape_write(&daemon_ctx, b, BLK_SZ, &r);
	harness_tape_write_fm(&daemon_ctx, 1, &r);
	harness_tape_rewind(&daemon_ctx, &r);

	erase(0, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "ERASE at BOT stat=%d",
		    r.sam_stat);
	TEST_CHECK_(read_position() == 0, "position %ld after ERASE (expected 0)",
		    read_position());
	check_blank("after ERASE at BOT");

	stop_drive();
}

void test_erase_write_protected(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_drive(1) == 0);

	erase(0, &r);
	TEST_CHECK_(sense_is(&r, 0x07, 0x27, 0x00),
		    "ERASE on write-protected media stat=%d sense %x/%02x/%02x (expected 7/27/00)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	stop_drive();
}

TEST_LIST = {
	{ "erase_mid_tape",             test_erase_mid_tape },
	{ "erase_long_keeps_filemarks", test_erase_long_keeps_filemarks },
	{ "erase_at_bot",               test_erase_at_bot },
	{ "erase_write_protected",      test_erase_write_protected },
	{ NULL, NULL }
};
