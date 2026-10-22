/*
 * Integration tests for LTO-8 Type M ("M8") media - markh794/mhvtl#297.
 *
 * M8 is LTO-7 media written in the LTO-8 Type M format: density code 5Dh,
 * "U-832M", 9 TB native (IBM LTO SCSI Reference GA32-0928-07, table 113,
 * p.142). Only LTO-8 drives read and write it; LTO-7 drives cannot read it
 * and LTO-9 drives neither read nor write it (GA32-0928-07 3.4, p.22). HP
 * LTO-8 drives read and write it too (HPE LTO-8 TRM Vol 2, "LTO-7 Type M
 * media for LTO-8 drives"). The medium type in the mode parameter header is
 * LTO-7's 78h on IBM drives (markh794/mhvtl#71, comment 941601643). On HP
 * drives mhVTL reports 00h for every data cartridge, its own value rather
 * than HP's (HPE LTO-8 TRM Vol 2 gives n8h, 88h for LTO-8), so the test
 * only requires M8 to report what the same HP drive reports for LTO-8.
 *
 * Creates an M8 cartridge with mktape -d LTO8M8, starts vtltape with
 * MHVTL_BACKEND=userland and loads it with vtlcmd; no kernel module or root.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"

#define BC_M8	 "E01001M8"
#define BC_L8	 "E01002L8"
#define RDS_LEN	 52 /* density support data block descriptor */

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

/* Start a drive with an M8 cartridge in the library and load it. */
static int start_drive(const char *vendor, const char *product)
{
	const struct drive_type dt = {
		product, vendor, product, "0001", BC_M8, "LTO8M8", 1, 1, 1
	};
	char resp[64];

	if (gen_test_env(&test_env, &dt) < 0)
		return -1;
	if (harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env) < 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	if (harness_load_tape(TEST_MINOR_TAPE, BC_M8) < 0)
		return -1;
	/* The queue is handled in order: once this answers, the load is done */
	return harness_vtlcmd(TEST_MINOR_TAPE, msg_mount_state, resp, sizeof(resp));
}

/* Load an LTO-8 cartridge in place of the M8 one */
static int swap_to_lto8(void)
{
	char mktape_path[256], cmd[1024], resp[64];

	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "%s%s -C %s -H %s -l %d -m %s -s 500 -t data -d LTO8 >/dev/null 2>&1",
		 gen_local_lib_prefix(mktape_path), mktape_path,
		 test_env.conf_dir, test_env.home_dir, TEST_MINOR_LIBRARY, BC_L8);
	if (system(cmd) != 0 || harness_unload_tape(TEST_MINOR_TAPE) < 0 ||
	    harness_load_tape(TEST_MINOR_TAPE, BC_L8) < 0)
		return -1;
	return harness_vtlcmd(TEST_MINOR_TAPE, msg_mount_state, resp, sizeof(resp));
}

static void stop_drive(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

/* TEST UNIT READY until the mount completes; returns the last result */
static void wait_ready(struct scsi_result *r)
{
	uint8_t tur[6] = { 0x00 };

	for (int i = 0; i < 50; i++) {
		harness_cdb(&daemon_ctx, tur, 6, r);
		if (r->sam_stat == SAM_STAT_GOOD)
			return;
		usleep(100000);
	}
}

static int report_density(int media, uint8_t *buf, size_t sz,
			  struct scsi_result *r)
{
	uint8_t cdb[10] = { 0x44, (uint8_t)(media ? 0x01 : 0x00), 0, 0, 0, 0, 0,
			    (uint8_t)(sz >> 8), (uint8_t)sz, 0 };

	memset(buf, 0, sz);
	return harness_send_cdb(&daemon_ctx, cdb, 10, r, buf, sz);
}

/* medium_type < 0: expect what the drive reports for an LTO-8 cartridge */
static void check_m8_read_write(const char *vendor, const char *product,
				int medium_type)
{
	uint8_t buf[1024], blk[512];
	uint8_t ms[6] = { 0x1a, 0x00, 0x3f, 0x00, 255, 0x00 };
	struct scsi_result r;
	int found = 0;
	uint8_t m8_type;

	TEST_ASSERT(start_drive(vendor, product) == 0);
	wait_ready(&r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "%s loads M8: TUR stat=%d",
		    product, r.sam_stat);

	/* MODE SENSE(6): medium type in header byte 1, density in the block
	 * descriptor, byte 0 */
	memset(buf, 0, sizeof(buf));
	TEST_CHECK(harness_send_cdb(&daemon_ctx, ms, 6, &r, buf, 255) == 0);
	TEST_CHECK(r.sam_stat == SAM_STAT_GOOD);
	m8_type = buf[1];
	if (medium_type >= 0)
		TEST_CHECK_(m8_type == medium_type,
			    "medium type 0x%02x (want 0x%02x)", m8_type,
			    medium_type);
	TEST_CHECK_(buf[3] >= 8 && buf[4] == 0x5d,
		    "block descriptor density 0x%02x (want 0x5d)", buf[4]);

	/* REPORT DENSITY SUPPORT, drive: 5Dh listed as writable */
	TEST_CHECK(report_density(0, buf, sizeof(buf), &r) == 0);
	TEST_CHECK(r.sam_stat == SAM_STAT_GOOD);
	for (int off = 4; off + RDS_LEN <= (int)sizeof(buf) &&
	     off < ((buf[0] << 8) | buf[1]) + 2; off += RDS_LEN)
		if (buf[off] == 0x5d && (buf[off + 2] & 0x80))
			found = 1;
	TEST_CHECK_(found, "drive reports density 0x5d with WRTOK");

	/* REPORT DENSITY SUPPORT, mounted medium: 5Dh, writable, U-832M */
	TEST_CHECK(report_density(1, buf, sizeof(buf), &r) == 0);
	TEST_CHECK(r.sam_stat == SAM_STAT_GOOD);
	TEST_CHECK_(buf[4] == 0x5d && (buf[6] & 0x80),
		    "medium density 0x%02x flags 0x%02x (want 0x5d, WRTOK)",
		    buf[4], buf[6]);
	TEST_CHECK_(!memcmp(&buf[4 + 24], "U-832M  ", 8),
		    "medium density name '%.8s'", &buf[4 + 24]);

	memset(blk, 0xa5, sizeof(blk));
	harness_tape_write(&daemon_ctx, blk, sizeof(blk), &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "WRITE to M8 stat=%d",
		    r.sam_stat);

	if (medium_type < 0) {
		TEST_CHECK(swap_to_lto8() == 0);
		wait_ready(&r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "%s loads LTO-8", product);
		memset(buf, 0, sizeof(buf));
		TEST_CHECK(harness_send_cdb(&daemon_ctx, ms, 6, &r, buf, 255) == 0);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && buf[4] == 0x5e,
			    "LTO-8 cartridge density 0x%02x", buf[4]);
		TEST_CHECK_(m8_type == buf[1],
			    "M8 medium type 0x%02x, LTO-8 0x%02x", m8_type, buf[1]);
	}

	stop_drive();
}

void test_m8_ibm_lto8(void)
{
	check_m8_read_write("IBM", "ULT3580-TD8", 0x78);
}

void test_m8_hp_lto8(void)
{
	check_m8_read_write("HP", "Ultrium 8-SCSI", -1);
}

/* The load is refused: the drive stays NOT READY, MEDIUM NOT PRESENT */
static void check_m8_refused(const char *vendor, const char *product)
{
	struct scsi_result r;

	TEST_ASSERT(start_drive(vendor, product) == 0);
	wait_ready(&r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION &&
		    (r.sense[2] & 0x0f) == 0x02 && r.sense[12] == 0x3a &&
		    r.sense[13] == 0x00,
		    "%s refuses M8: stat=%d sense %x/%02x/%02x (want 2/3a/00)",
		    product, r.sam_stat, r.sense[2] & 0x0f, r.sense[12],
		    r.sense[13]);
	stop_drive();
}

void test_m8_refused_ibm_lto7(void)
{
	check_m8_refused("IBM", "ULT3580-TD7");
}

void test_m8_refused_ibm_lto9(void)
{
	check_m8_refused("IBM", "ULT3580-TD9");
}

void test_m8_refused_hp_lto7(void)
{
	check_m8_refused("HP", "Ultrium 7-SCSI");
}

TEST_LIST = {
	{ "m8_ibm_lto8",	  test_m8_ibm_lto8 },
	{ "m8_hp_lto8",		  test_m8_hp_lto8 },
	{ "m8_refused_ibm_lto7", test_m8_refused_ibm_lto7 },
	{ "m8_refused_ibm_lto9", test_m8_refused_ibm_lto9 },
	{ "m8_refused_hp_lto7",	  test_m8_refused_hp_lto7 },
	{ NULL, NULL }
};
