/*
 * Integration tests for LOCATE(10)/LOCATE(16).
 *
 * Starts vtltape (LTO-8) with MHVTL_BACKEND=userland against a generated
 * device.conf and checks that LOCATE does not touch cartridge state before it
 * has confirmed a tape is loaded and the CDB fields are in range, and that a
 * logical file identifier resolves to the right block.
 *
 * Regression for markh794/mhvtl#329: LOCATE(16) with a logical file identifier
 * dereferenced filemarks[partition][file] with no guard, crashing vtltape on
 * an empty drive or on a file number past the last filemark.
 *
 * Reference: IBM LTO SCSI Reference GA32-0928-07, LOCATE(16) (destination
 * type, logical file identifier) and SSC-3 r01c logical position model.
 */
#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

static int start(void)
{
	const struct drive_type *dt;
	struct scsi_result r;

	for (dt = ALL_DRIVES; dt->name; dt++)
		if (strcmp(dt->name, "LTO-8") == 0)
			break;
	if (!dt->name || gen_test_env(&test_env, dt) < 0)
		return -1;
	if (harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env) != 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	if (harness_load_tape(TEST_MINOR_TAPE, dt->media_barcode) != 0)
		return -1;
	for (int i = 0; i < 100; i++) {
		uint8_t tur[6] = { 0x00 };

		harness_cdb(&daemon_ctx, tur, 6, &r);
		if (r.sam_stat == SAM_STAT_GOOD)
			return 0;
		usleep(100000);
	}
	return -1;
}

static void stop(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

static void write_blocks(int n)
{
	uint8_t buf[512];
	struct scsi_result r;

	memset(buf, 0x5a, sizeof(buf));
	for (int i = 0; i < n; i++) {
		harness_tape_write(&daemon_ctx, buf, sizeof(buf), &r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "WRITE sam_stat=%d",
			    r.sam_stat);
	}
}

/* LOCATE(16), destination type = logical file identifier (0b01) */
static void locate_file(uint64_t file, struct scsi_result *r)
{
	uint8_t cdb[16] = {
		0x92, 0x08,
		0, 0,
		(file >> 56) & 0xff, (file >> 48) & 0xff,
		(file >> 40) & 0xff, (file >> 32) & 0xff,
		(file >> 24) & 0xff, (file >> 16) & 0xff,
		(file >> 8) & 0xff, file & 0xff,
		0, 0, 0, 0
	};
	harness_cdb(&daemon_ctx, cdb, 16, r);
}

/* LOCATE(16), CP set, change to the given partition, block 0 */
static void locate_partition_block0(uint8_t partition, struct scsi_result *r)
{
	uint8_t cdb[16] = {
		0x92, 0x02, /* CP */
		0, partition, /* byte 3 is the partition */
		0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0
	};
	harness_cdb(&daemon_ctx, cdb, 16, r);
}

/* READ POSITION short form: returns first block location */
static uint32_t read_position(void)
{
	uint8_t cdb[10] = { 0x34 };
	struct scsi_result r;

	harness_cdb(&daemon_ctx, cdb, 10, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "READ POSITION sam_stat=%d",
		    r.sam_stat);
	return (uint32_t)harness_data_buf[4] << 24 | harness_data_buf[5] << 16 |
	       harness_data_buf[6] << 8 | harness_data_buf[7];
}

/* Is the drive still alive and answering after a bad LOCATE? */
static int drive_alive(void)
{
	uint8_t tur[6] = { 0x00 };
	struct scsi_result r;

	return harness_cdb(&daemon_ctx, tur, 6, &r) == 0;
}

/*
 * #329: LOCATE(16) by file identifier on an empty drive must answer
 * NOT READY, not dereference the freed/NULL filemark table and crash.
 */
void test_locate_empty_drive(void)
{
	struct scsi_result r;

	if (!TEST_CHECK(start() == 0)) {
		stop();
		return;
	}

	TEST_CHECK(harness_unload_tape(TEST_MINOR_TAPE) == 0);
	harness_clear_ua(&daemon_ctx);

	locate_file(3, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION,
		    "LOCATE file on empty drive sam_stat=%d", r.sam_stat);
	TEST_CHECK_((r.sense[2] & 0x0f) == 0x02 && r.sense[12] == 0x3a &&
		    r.sense[13] == 0x00,
		    "sense %x/%02x/%02x (expected 2/3a/00 NOT READY)",
		    r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	TEST_CHECK_(drive_alive(), "vtltape still answering after LOCATE");

	stop();
}

/*
 * 3 blocks, FM, 2 blocks, FM, rewind. Filemarks sit at objects 3 and 6.
 * LOCATE to logical file 0 is BOP; file 1 is the first block of the second
 * file (right after the first filemark, object 4).
 */
void test_locate_by_file_id(void)
{
	struct scsi_result r;
	uint32_t pos;

	if (!TEST_CHECK(start() == 0)) {
		stop();
		return;
	}

	write_blocks(3);
	harness_tape_write_fm(&daemon_ctx, 1, &r);
	write_blocks(2);
	harness_tape_write_fm(&daemon_ctx, 1, &r);
	harness_tape_rewind(&daemon_ctx, &r);

	locate_file(1, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "LOCATE file 1 sam_stat=%d",
		    r.sam_stat);
	pos = read_position();
	TEST_CHECK_(pos == 4, "position after LOCATE file 1 = %u (expected 4)",
		    pos);

	locate_file(0, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "LOCATE file 0 sam_stat=%d",
		    r.sam_stat);
	pos = read_position();
	TEST_CHECK_(pos == 0, "position after LOCATE file 0 = %u (expected 0)",
		    pos);

	stop();
}

/*
 * #329: a file identifier past the last filemark must report end-of-data,
 * not read past the end of the filemark array.
 */
void test_locate_file_past_eod(void)
{
	struct scsi_result r;

	if (!TEST_CHECK(start() == 0)) {
		stop();
		return;
	}

	write_blocks(2);
	harness_tape_write_fm(&daemon_ctx, 1, &r);
	harness_tape_rewind(&daemon_ctx, &r);

	locate_file(9, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION,
		    "LOCATE file 9 sam_stat=%d", r.sam_stat);
	TEST_CHECK_((r.sense[2] & 0x0f) == 0x08 && r.sense[12] == 0x00 &&
		    r.sense[13] == 0x05,
		    "sense %x/%02x/%02x (expected 8/00/05 BLANK CHECK/EOD)",
		    r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	/* a huge value that would alias a low index if narrowed to 32 bits */
	locate_file((uint64_t)1 << 32, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION,
		    "LOCATE file 2^32 sam_stat=%d", r.sam_stat);

	TEST_CHECK_(drive_alive(), "vtltape still answering after LOCATE");

	stop();
}

/*
 * #329: LOCATE(16) with CP set and a partition number past the cartridge's
 * partition count must be an ILLEGAL REQUEST, checked before the partition is
 * used to index the per-partition tables.
 */
void test_locate_bad_partition(void)
{
	struct scsi_result r;

	if (!TEST_CHECK(start() == 0)) {
		stop();
		return;
	}

	harness_clear_ua(&daemon_ctx);

	locate_partition_block0(5, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION,
		    "LOCATE partition 5 sam_stat=%d", r.sam_stat);
	TEST_CHECK_((r.sense[2] & 0x0f) == 0x05 && r.sense[12] == 0x24 &&
		    r.sense[13] == 0x00,
		    "sense %x/%02x/%02x (expected 5/24/00 ILLEGAL REQUEST)",
		    r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	TEST_CHECK_(drive_alive(), "vtltape still answering after LOCATE");

	stop();
}

TEST_LIST = {
	{ "locate_empty_drive",   test_locate_empty_drive },
	{ "locate_by_file_id",    test_locate_by_file_id },
	{ "locate_file_past_eod", test_locate_file_past_eod },
	{ "locate_bad_partition", test_locate_bad_partition },
	{ NULL, NULL }
};
