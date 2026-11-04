/*
 * test_write_short_data.c — a failed or short data-out transfer changes
 * nothing (#345).
 *
 * When the transport delivers fewer bytes than the command asked for (the
 * TCMU handler went away, an ioctl failed), the data buffer still holds an
 * earlier command's bytes. The command must end with CHECK CONDITION,
 * ABORTED COMMAND / DATA PHASE ERROR (GA32-0928-07 Table B.10, p.484) and
 * act on nothing:
 *   - WRITE(6) writes no block; INFORMATION is the requested transfer length
 *     (GA32-0928-07 4.12.2 Table 17, pp.50-51, the "no write" rows);
 *   - SECURITY PROTOCOL OUT and PERSISTENT RESERVE OUT parse nothing.
 *
 * The harness sends min(supplied, requested) bytes of DATA-OUT, so a short
 * buffer reproduces the failure.
 *
 * Usage: ./test_write_short_data
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"

#define BLK_SZ 1024

#define KEY_ABORTED_COMMAND 0x0b
#define SENSE_FILEMARK		0x80

static struct harness_daemon d;
static struct test_env env;
static uint8_t good[BLK_SZ];
static uint8_t other[BLK_SZ];
static uint8_t rbuf[4 * BLK_SZ];

static int setup(void)
{
	const struct drive_type *dt;

	for (dt = ALL_DRIVES; dt->name; dt++)
		if (!strcmp(dt->name, "LTO-8"))
			break;
	if (!dt->name || gen_test_env(&env, dt) < 0)
		return -1;
	if (harness_start_env(&d, TEST_MINOR_TAPE, "tape", &env) < 0)
		return -1;
	harness_clear_ua(&d);
	if (harness_load_tape(TEST_MINOR_TAPE, dt->media_barcode) < 0)
		return -1;
	for (int i = 0; i < 100; i++) {
		uint8_t tur[6] = { 0x00 };
		struct scsi_result r;

		harness_cdb(&d, tur, 6, &r);
		if (r.sam_stat == SAM_STAT_GOOD)
			return 0;
		usleep(100000);
	}
	return -1;
}

static void teardown(void)
{
	harness_stop(&d);
	gen_cleanup_env(&env);
}

static int is_data_phase_error(const struct scsi_result *r)
{
	return r->sam_stat == SAM_STAT_CHECK_CONDITION &&
	       (r->sense[2] & 0x0f) == KEY_ABORTED_COMMAND &&
	       r->sense[12] == 0x4b && r->sense[13] == 0x00;
}

/* WRITE(6): 'len' is the CDB transfer length, 'supplied' the DATA-OUT bytes */
static void write6(uint8_t fixed, uint32_t len, const uint8_t *data,
		   uint32_t supplied, struct scsi_result *r)
{
	uint8_t cdb[6] = { 0x0a, fixed, (len >> 16) & 0xff,
			   (len >> 8) & 0xff, len & 0xff, 0x00 };

	harness_send_cdb_with_data_out(&d, cdb, 6, data, supplied, r);
}

/* MODE SELECT(6) the block descriptor BLOCK LENGTH, keeping density */
static int set_mode_block_length(uint32_t blk_len)
{
	uint8_t sense_cdb[6] = { 0x1a, 0x00, 0x10, 0x00, 0xff, 0x00 };
	uint8_t sel_cdb[6] = { 0x15, 0x10, 0x00, 0x00, 12, 0x00 };
	uint8_t ms[256], param[12];
	struct scsi_result r;

	harness_send_cdb(&d, sense_cdb, 6, &r, ms, sizeof(ms));
	if (r.sam_stat != SAM_STAT_GOOD || ms[3] != 8)
		return -1;

	memset(param, 0, sizeof(param));
	param[2] = ms[2] & 0x7f;
	param[3] = 8;
	memcpy(&param[4], &ms[4], 8);
	put_unaligned_be24(blk_len, &param[9]);

	harness_send_cdb_with_data_out(&d, sel_cdb, 6, param, sizeof(param), &r);
	return r.sam_stat == SAM_STAT_GOOD ? 0 : -1;
}

static void read6(uint32_t len, struct scsi_result *r)
{
	uint8_t cdb[6] = { 0x08, 0x00, (len >> 16) & 0xff,
			   (len >> 8) & 0xff, len & 0xff, 0x00 };

	memset(rbuf, 0xde, sizeof(rbuf));
	harness_send_cdb(&d, cdb, 6, r, rbuf, sizeof(rbuf));
}

void test_write_short_data(void)
{
	struct scsi_result r;

	for (int i = 0; i < BLK_SZ; i++) {
		good[i] = (uint8_t)(0x5a ^ i);
		other[i] = (uint8_t)(0xa5 ^ (i * 7));
	}

	if (!TEST_CHECK_(setup() == 0, "vtltape LTO-8 ready")) {
		teardown();
		return;
	}

	harness_tape_rewind(&d, &r);
	write6(0x00, BLK_SZ, good, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "WRITE 1 KiB block");

	TEST_CASE("variable, half the data");
	write6(0x00, BLK_SZ, other, BLK_SZ / 2, &r);
	TEST_CHECK_(is_data_phase_error(&r),
		    "sam_stat=%d sense %02x/%02x/%02x (expected 0b/4b/00)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	TEST_CHECK_(r.sense[0] & 0x80, "VALID set");
	TEST_CHECK_(get_unaligned_be32(&r.sense[3]) == BLK_SZ,
		    "INFO=%u (expected %u)", get_unaligned_be32(&r.sense[3]), BLK_SZ);

	TEST_CASE("variable, no data");
	write6(0x00, BLK_SZ, other, 0, &r);
	TEST_CHECK_(is_data_phase_error(&r),
		    "sam_stat=%d sense %02x/%02x/%02x (expected 0b/4b/00)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	TEST_CASE("fixed, 2 blocks, one block of data");
	if (TEST_CHECK_(set_mode_block_length(BLK_SZ) == 0, "block length 1024")) {
		write6(0x01, 2, other, BLK_SZ, &r);
		TEST_CHECK_(is_data_phase_error(&r),
			    "sam_stat=%d sense %02x/%02x/%02x (expected 0b/4b/00)",
			    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
		TEST_CHECK_(get_unaligned_be32(&r.sense[3]) == 2,
			    "INFO=%u (expected 2 blocks)", get_unaligned_be32(&r.sense[3]));
		TEST_CHECK_(set_mode_block_length(0) == 0, "back to variable");
	}

	TEST_CASE("nothing was written");
	harness_tape_write_fm(&d, 1, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "WRITE FILEMARKS");
	harness_tape_rewind(&d, &r);
	read6(BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && memcmp(rbuf, good, BLK_SZ) == 0,
		    "first block is the good one (sam_stat=%d)", r.sam_stat);
	read6(BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION &&
		    (r.sense[2] & SENSE_FILEMARK),
		    "next is the filemark, no stale block (sam_stat=%d sense[2]=0x%02x data_len=%u)",
		    r.sam_stat, r.sense[2], r.data_len);

	TEST_CASE("SECURITY PROTOCOL OUT, short");
	{
		/* Tape Data Encryption, Set Data Encryption page, 52 bytes */
		uint8_t cdb[12] = { 0xb5, 0x20, 0x00, 0x10, 0x00, 0x00,
				    0x00, 0x00, 0x00, 52, 0x00, 0x00 };

		harness_send_cdb_with_data_out(&d, cdb, 12, other, 8, &r);
		TEST_CHECK_(is_data_phase_error(&r),
			    "sam_stat=%d sense %02x/%02x/%02x (expected 0b/4b/00)",
			    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	}

	TEST_CASE("PERSISTENT RESERVE OUT, short");
	{
		/* REGISTER, parameter list length 24 */
		uint8_t cdb[10] = { 0x5f, 0x00, 0x00, 0x00, 0x00,
				    0x00, 0x00, 0x00, 24, 0x00 };

		harness_send_cdb_with_data_out(&d, cdb, 10, other, 8, &r);
		TEST_CHECK_(is_data_phase_error(&r),
			    "sam_stat=%d sense %02x/%02x/%02x (expected 0b/4b/00)",
			    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	}

	TEST_CASE("a complete WRITE still works");
	harness_tape_rewind(&d, &r);
	write6(0x00, BLK_SZ, other, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "WRITE 1 KiB block");

	teardown();
}

TEST_LIST = {
	{ "write_short_data", test_write_short_data },
	{ NULL, NULL }
};
