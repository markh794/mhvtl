/*
 * test_read_sili.c — READ(6) SILI handling for incorrect-length blocks.
 *
 * Variable mode (FIXED=0), per IBM LTO SCSI Reference GA32-0928-07
 * 4.12.1 p.49 and Table 17 p.50:
 *   - underlength, SILI=0: CHECK CONDITION 0/0000, ILI, INFO = request - block
 *   - underlength, SILI=1: GOOD
 *   - overlength,  SILI=1, mode BLOCK LENGTH 0:     GOOD
 *   - overlength,  SILI=1, mode BLOCK LENGTH non-0: CHECK CONDITION, ILI
 *
 * Usage: ./test_read_sili
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"

#define BLK_SZ   1024
#define LONG_RQ  65536
#define SHORT_RQ 512

#define SILI_BIT 0x02

static struct harness_daemon d;
static struct test_env env;
static uint8_t wbuf[BLK_SZ];
static uint8_t rbuf[LONG_RQ];

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

static void read6(uint8_t byte1, uint32_t len, struct scsi_result *r)
{
	uint8_t cdb[6] = { 0x08, byte1, (len >> 16) & 0xff,
			   (len >> 8) & 0xff, len & 0xff, 0x00 };
	struct scsi_result rw;

	harness_tape_rewind(&d, &rw);
	memset(rbuf, 0xde, sizeof(rbuf));
	harness_send_cdb(&d, cdb, 6, r, rbuf, sizeof(rbuf));
}

static int has_ili(const struct scsi_result *r)
{
	return r->sam_stat == SAM_STAT_CHECK_CONDITION &&
	       (r->sense[2] & 0x2f) == 0x20;
}

/* Set the mode parameter block descriptor BLOCK LENGTH via MODE SELECT(6),
 * keeping the density and header fields MODE SENSE(6) reports. */
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
	param[2] = ms[2] & 0x7f;	/* WP is reserved for MODE SELECT */
	param[3] = 8;
	memcpy(&param[4], &ms[4], 8);
	put_unaligned_be24(blk_len, &param[9]);

	harness_send_cdb_with_data_out(&d, sel_cdb, 6, param, sizeof(param), &r);
	return r.sam_stat == SAM_STAT_GOOD ? 0 : -1;
}

void test_read_sili(void)
{
	struct scsi_result r;

	memset(wbuf, 0, sizeof(wbuf));
	for (int i = 0; i < BLK_SZ; i++)
		wbuf[i] = (uint8_t)(0x5a ^ i);

	if (!TEST_CHECK_(setup() == 0, "vtltape LTO-8 ready")) {
		teardown();
		return;
	}

	harness_tape_write(&d, wbuf, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "WRITE 1 KiB block");
	harness_tape_write_fm(&d, 1, &r);

	TEST_CASE("underlength, SILI=0");
	read6(0x00, LONG_RQ, &r);
	TEST_CHECK_(has_ili(&r), "sam_stat=%d sense[2]=0x%02x (expected ILI)",
		    r.sam_stat, r.sense[2]);
	TEST_CHECK_(get_unaligned_be32(&r.sense[3]) == LONG_RQ - BLK_SZ,
		    "INFO=%u (expected %u)",
		    get_unaligned_be32(&r.sense[3]), LONG_RQ - BLK_SZ);
	TEST_CHECK_(r.data_len == BLK_SZ, "data_len=%u", r.data_len);

	TEST_CASE("underlength, SILI=1");
	read6(SILI_BIT, LONG_RQ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
		    "sam_stat=%d sense[2]=0x%02x INFO=%u (expected GOOD)",
		    r.sam_stat, r.sense[2], get_unaligned_be32(&r.sense[3]));
	TEST_CHECK_(r.data_len == BLK_SZ, "data_len=%u", r.data_len);
	TEST_CHECK_(memcmp(rbuf, wbuf, BLK_SZ) == 0, "data matches");

	TEST_CASE("overlength, SILI=0");
	read6(0x00, SHORT_RQ, &r);
	TEST_CHECK_(has_ili(&r), "sam_stat=%d sense[2]=0x%02x (expected ILI)",
		    r.sam_stat, r.sense[2]);
	TEST_CHECK_((int32_t)get_unaligned_be32(&r.sense[3]) == SHORT_RQ - BLK_SZ,
		    "INFO=%d (expected %d)",
		    (int32_t)get_unaligned_be32(&r.sense[3]), SHORT_RQ - BLK_SZ);

	TEST_CASE("overlength, SILI=1, BLOCK LENGTH 0");
	read6(SILI_BIT, SHORT_RQ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
		    "sam_stat=%d sense[2]=0x%02x (expected GOOD)",
		    r.sam_stat, r.sense[2]);
	TEST_CHECK_(r.data_len == SHORT_RQ, "data_len=%u", r.data_len);

	TEST_CASE("overlength, SILI=1, BLOCK LENGTH non-0");
	TEST_CHECK_(set_mode_block_length(SHORT_RQ) == 0,
		    "MODE SELECT block length %u", SHORT_RQ);
	read6(SILI_BIT, SHORT_RQ, &r);
	TEST_CHECK_(has_ili(&r), "sam_stat=%d sense[2]=0x%02x (expected ILI)",
		    r.sam_stat, r.sense[2]);
	TEST_CHECK_((int32_t)get_unaligned_be32(&r.sense[3]) == SHORT_RQ - BLK_SZ,
		    "INFO=%d (expected %d)",
		    (int32_t)get_unaligned_be32(&r.sense[3]), SHORT_RQ - BLK_SZ);

	teardown();
}

TEST_LIST = {
	{ "read_sili", test_read_sili },
	{ NULL, NULL }
};
