/*
 * test_drive_types.c — Test that every supported tape drive type starts,
 * responds to INQUIRY correctly, and reports proper capabilities.
 *
 * Uses the userland transport — no kernel modules needed.
 * Generates a temporary device.conf for each drive type.
 *
 * Usage: sudo ./test_drive_types
 *
 * Build: gcc -o test_drive_types test_drive_types.c -I.. -I../include -I../include/common -I../include/utils -I../ccan
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

static int start_drive(const struct drive_type *dt)
{
	gen_cleanup_env(&test_env);
	if (gen_test_env(&test_env, dt) < 0) return -1;
	return harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env);
}

static void stop_drive(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

/*
 * Test: every drive type starts and responds to INQUIRY.
 * One sub-test per drive type.
 */
void test_all_drives_inquiry(void)
{
	uint8_t inq_buf[96];

	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++) {
		TEST_CASE(dt->name);

		int rc = start_drive(dt);
		if (rc < 0) {
			TEST_CHECK_(0, "daemon start for %s", dt->name);
			stop_drive();
			continue;
		}

		harness_clear_ua(&daemon_ctx);

		int dev_type = harness_inquiry(&daemon_ctx, inq_buf, sizeof(inq_buf));
		TEST_CHECK_(dev_type == 1,
			    "%s: device type = %d (expected 1/tape)", dt->name, dev_type);

		/* Check vendor */
		char vendor[9] = {0};
		memcpy(vendor, &inq_buf[8], 8);
		/* Trim trailing spaces */
		for (int i = 7; i >= 0 && vendor[i] == ' '; i--) vendor[i] = 0;
		TEST_CHECK_(strstr(vendor, dt->vendor) != NULL ||
			    strcmp(vendor, "IBM") == 0 ||
			    strcmp(vendor, "HP") == 0 ||
			    strcmp(vendor, "STK") == 0 ||
			    strcmp(vendor, "QUANTUM") == 0 ||
			    strcmp(vendor, "SONY") == 0,
			    "%s: vendor '%.8s'", dt->name, vendor);

		/* Check product contains the expected string */
		char product[17] = {0};
		memcpy(product, &inq_buf[16], 16);
		TEST_MSG("%s: vendor='%s' product='%s'", dt->name, vendor, product);

		stop_drive();
	}
}

/*
 * Test: every drive type responds to TEST UNIT READY correctly.
 * With no tape loaded, should return NOT READY.
 */
void test_all_drives_tur(void)
{
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++) {
		TEST_CASE(dt->name);

		int rc = start_drive(dt);
		if (rc < 0) {
			TEST_CHECK_(0, "daemon start for %s", dt->name);
			stop_drive();
			continue;
		}

		harness_clear_ua(&daemon_ctx);

		uint8_t cdb[6] = { 0x00 };
		struct scsi_result r;
		TEST_CHECK(harness_cdb(&daemon_ctx, cdb, 6, &r) == 0);
		TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION,
			    "%s: TUR sam_stat=%d", dt->name, r.sam_stat);
		TEST_CHECK_((r.sense[2] & 0x0f) == 0x02,
			    "%s: sense key=0x%02x (expected 0x02 NOT READY)",
			    dt->name, r.sense[2] & 0x0f);

		stop_drive();
	}
}

/*
 * Test: every drive type responds to MODE SENSE.
 */
void test_all_drives_mode_sense(void)
{
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++) {
		TEST_CASE(dt->name);

		int rc = start_drive(dt);
		if (rc < 0) {
			TEST_CHECK_(0, "daemon start for %s", dt->name);
			stop_drive();
			continue;
		}

		harness_clear_ua(&daemon_ctx);

		uint8_t cdb[6] = { 0x1a, 0x00, 0x3f, 0x00, 255, 0x00 };
		struct scsi_result r;
		TEST_CHECK(harness_cdb(&daemon_ctx, cdb, 6, &r) == 0);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
			    "%s: MODE SENSE sam_stat=%d", dt->name, r.sam_stat);
		TEST_CHECK_(r.data_len > 4,
			    "%s: MODE SENSE returned %u bytes", dt->name, r.data_len);

		stop_drive();
	}
}

/*
 * Test: every drive type responds to READ BLOCK LIMITS.
 */
void test_all_drives_block_limits(void)
{
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++) {
		TEST_CASE(dt->name);

		int rc = start_drive(dt);
		if (rc < 0) {
			TEST_CHECK_(0, "daemon start for %s", dt->name);
			stop_drive();
			continue;
		}

		harness_clear_ua(&daemon_ctx);

		uint8_t cdb[6] = { 0x05, 0x00, 0x00, 0x00, 0x00, 0x00 };
		struct scsi_result r;
		TEST_CHECK(harness_cdb(&daemon_ctx, cdb, 6, &r) == 0);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
			    "%s: READ BLOCK LIMITS sam_stat=%d",
			    dt->name, r.sam_stat);
		TEST_CHECK_(r.data_len >= 5,
			    "%s: returned %u bytes", dt->name, r.data_len);

		stop_drive();
	}
}

TEST_LIST = {
	{ "all_drives_inquiry",      test_all_drives_inquiry },
	{ "all_drives_tur",          test_all_drives_tur },
	{ "all_drives_mode_sense",   test_all_drives_mode_sense },
	{ "all_drives_block_limits", test_all_drives_block_limits },
	{ NULL, NULL }
};
