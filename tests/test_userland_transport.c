/*
 * test_userland_transport.c — End-to-end test for mhvtl via userland transport.
 *
 * Starts vtltape/vtllibrary with MHVTL_BACKEND=userland, connects to the
 * daemon's socket, and sends SCSI CDBs to exercise the full command
 * processing pipeline without any kernel modules.
 *
 * Usage: ./test_userland_transport [-m minor] [-t tape|library]
 *
 * Build: gcc -o test_userland_transport test_userland_transport.c -I../include -I../include/common
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
static unsigned test_minor = 11;

static int prepare_env(unsigned minor, const char *type)
{
	char path[512];
	FILE *fp;
	unsigned library_minor;
	unsigned tape_minor;
	const char *vendor = "IBM";
	const char *product = "ULT3580-TD1";
	const char *revision = "0100";
	const char *barcode = "E01001L1";

	memset(&test_env, 0, sizeof(test_env));
	snprintf(test_env.base_dir, sizeof(test_env.base_dir),
		 "/tmp/mhvtl-userland-XXXXXX");
	if (!mkdtemp(test_env.base_dir))
		return -1;

	snprintf(test_env.conf_dir, sizeof(test_env.conf_dir), "%s/conf",
		 test_env.base_dir);
	snprintf(test_env.home_dir, sizeof(test_env.home_dir), "%s/home",
		 test_env.base_dir);
	snprintf(test_env.lock_dir, sizeof(test_env.lock_dir), "%s/lock",
		 test_env.base_dir);
	snprintf(test_env.run_dir, sizeof(test_env.run_dir), "%s/run",
		 test_env.base_dir);

	mkdir(test_env.conf_dir, 0755);
	mkdir(test_env.home_dir, 0755);
	mkdir(test_env.lock_dir, 0755);
	mkdir(test_env.run_dir, 0755);

	library_minor = (strcmp(type, "library") == 0) ? minor : minor - 1;
	tape_minor = (strcmp(type, "library") == 0) ? minor + 1 : minor;

	snprintf(path, sizeof(path), "%s/mhvtl.conf", test_env.conf_dir);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp,
		"MHVTL_CONFIG_PATH=%s\n"
		"MHVTL_HOME_PATH=%s\n",
		test_env.conf_dir, test_env.home_dir);
	fclose(fp);

	snprintf(path, sizeof(path), "%s/device.conf", test_env.conf_dir);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp,
		"VERSION: 5\n"
		"\n"
		"Library: %u CHANNEL: 00 TARGET: 00 LUN: 00\n"
		" Vendor identification: STK\n"
		" Product identification: L700\n"
		" Product revision level: 0108\n"
		" Unit serial number: TESTLIB00\n"
		" NAA: 50:11:22:33:44:55:66:00\n"
		" Home directory: %s\n"
		"\n"
		"Drive: %u CHANNEL: 00 TARGET: 01 LUN: 00\n"
		" Library ID: %u Slot: 1\n"
		" Vendor identification: %-8s\n"
		" Product identification: %-16s\n"
		" Product revision level: %s\n"
		" Unit serial number: XYZZY_%02u\n"
		" NAA: 50:11:22:33:44:55:66:01\n"
		"\n",
		library_minor, test_env.home_dir,
		tape_minor, library_minor,
		vendor, product, revision, tape_minor);
	fclose(fp);

	snprintf(path, sizeof(path), "%s/library_contents.%u",
		 test_env.conf_dir, library_minor);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp,
		"Drive 1: %s\n"
		"Slot 1: %s\n"
		"Slot 2:\n"
		"MAP 1:\n",
		barcode, barcode);
	fclose(fp);

	char mktape_path[256];
	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;
	snprintf(path, sizeof(path),
		 "%s%s -C %s -H %s -l %u -m %s -s 500 -t data -d LTO1 >/dev/null 2>&1",
		 gen_local_lib_prefix(mktape_path), mktape_path,
		 test_env.conf_dir, test_env.home_dir, library_minor, barcode);
	return system(path);
}

static int start_daemon(unsigned minor, const char *type)
{
	gen_cleanup_env(&test_env);
	if (prepare_env(minor, type) != 0)
		return -1;
	if (harness_start_env(&daemon_ctx, minor, type, &test_env) != 0)
		return -1;
	return 0;
}

static void stop_daemon(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

/* ---- Helpers ---- */

/* Clear power-on UNIT ATTENTION by sending a TUR */
static void clear_unit_attention(void)
{
	uint8_t cdb[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	struct scsi_result result;
	harness_cdb(&daemon_ctx, cdb, 6, &result);
}

/* ---- Tests ---- */

void test_daemon_starts(void)
{
	TEST_CHECK(start_daemon(test_minor, "tape") == 0);
	TEST_CHECK(daemon_ctx.sock_fd >= 0);
	TEST_CHECK(daemon_ctx.pid > 0);

	/* Verify daemon is alive */
	TEST_CHECK(kill(daemon_ctx.pid, 0) == 0);

	stop_daemon();
}

void test_inquiry(void)
{
	TEST_CHECK(start_daemon(test_minor, "tape") == 0);
	clear_unit_attention();

	/* INQUIRY: opcode 0x12, page 0, alloc_len 96 */
	uint8_t cdb[6] = { 0x12, 0x00, 0x00, 0x00, 96, 0x00 };
	struct scsi_result result;
	uint8_t data[4096];

	TEST_CHECK(harness_send_cdb(&daemon_ctx, cdb, 6, &result,
				   data, sizeof(data)) == 0);
	TEST_CHECK(result.sam_stat == SAM_STAT_GOOD);
	TEST_CHECK(result.data_len >= 36);

	/* Check peripheral device type = 1 (tape) */
	TEST_CHECK((data[0] & 0x1f) == 1);

	/* Check vendor (bytes 8-15) */
	TEST_MSG("Vendor: %.8s", &data[8]);
	TEST_CHECK(memcmp(&data[8], "IBM     ", 8) == 0);

	/* Check product (bytes 16-31) */
	TEST_MSG("Product: %.16s", &data[16]);
	/* Should contain ULT3580 */
	TEST_CHECK(strstr((char *)&data[16], "ULT3580") != NULL);

	stop_daemon();
}

void test_test_unit_ready(void)
{
	TEST_CHECK(start_daemon(test_minor, "tape") == 0);

	uint8_t cdb[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	struct scsi_result result;

	/* First TUR gets UNIT ATTENTION (power-on reset) — clear it */
	TEST_CHECK(harness_cdb(&daemon_ctx, cdb, 6, &result) == 0);
	TEST_CHECK(result.sam_stat == SAM_STAT_CHECK_CONDITION);

	/* Second TUR: no tape loaded → NOT READY */
	TEST_CHECK(harness_cdb(&daemon_ctx, cdb, 6, &result) == 0);
	TEST_CHECK(result.sam_stat == SAM_STAT_CHECK_CONDITION);
	TEST_CHECK_((result.sense[2] & 0x0f) == 0x02,
		    "sense key: expected 0x02 (NOT READY), got 0x%02x",
		    result.sense[2] & 0x0f);

	stop_daemon();
}

void test_mode_sense(void)
{
	TEST_CHECK(start_daemon(test_minor, "tape") == 0);
	clear_unit_attention();

	/* MODE SENSE(6): opcode 0x1a, page 0x3f (all pages) */
	uint8_t cdb[6] = { 0x1a, 0x00, 0x3f, 0x00, 255, 0x00 };
	struct scsi_result result;
	uint8_t data[4096];

	TEST_CHECK(harness_send_cdb(&daemon_ctx, cdb, 6, &result,
				   data, sizeof(data)) == 0);
	TEST_CHECK(result.sam_stat == SAM_STAT_GOOD);
	TEST_CHECK(result.data_len > 4);
	TEST_MSG("Mode sense returned %u bytes", result.data_len);

	stop_daemon();
}

void test_read_block_limits(void)
{
	TEST_CHECK(start_daemon(test_minor, "tape") == 0);
	clear_unit_attention();

	/* READ BLOCK LIMITS: opcode 0x05 */
	uint8_t cdb[6] = { 0x05, 0x00, 0x00, 0x00, 0x00, 0x00 };
	struct scsi_result result;
	uint8_t data[4096];

	TEST_CHECK(harness_send_cdb(&daemon_ctx, cdb, 6, &result,
				   data, sizeof(data)) == 0);
	TEST_CHECK(result.sam_stat == SAM_STAT_GOOD);
	TEST_CHECK(result.data_len >= 5);

	stop_daemon();
}

void test_library_inquiry(void)
{
	TEST_CHECK(start_daemon(10, "library") == 0);
	clear_unit_attention();

	uint8_t cdb[6] = { 0x12, 0x00, 0x00, 0x00, 96, 0x00 };
	struct scsi_result result;
	uint8_t data[4096];

	TEST_CHECK(harness_send_cdb(&daemon_ctx, cdb, 6, &result,
				   data, sizeof(data)) == 0);
	TEST_CHECK(result.sam_stat == SAM_STAT_GOOD);
	TEST_CHECK(result.data_len >= 36);

	/* Check peripheral device type = 8 (medium changer) */
	TEST_CHECK((data[0] & 0x1f) == 8);

	TEST_MSG("Vendor: %.8s", &data[8]);
	TEST_MSG("Product: %.16s", &data[16]);

	stop_daemon();
}

void test_library_read_element_status(void)
{
	TEST_CHECK(start_daemon(10, "library") == 0);
	clear_unit_attention();

	/* READ ELEMENT STATUS: opcode 0xb8 */
	uint8_t cdb[12] = {
		0xb8,           /* opcode */
		0x10,           /* element type: all, voltag */
		0x00, 0x00,     /* starting element */
		0x00, 0xff,     /* number of elements */
		0x00,           /* reserved */
		0x00, 0x0f, 0xe8, /* allocation length (4072) */
		0x00, 0x00
	};
	struct scsi_result result;
	uint8_t data[4096];

	TEST_CHECK(harness_send_cdb(&daemon_ctx, cdb, 12, &result,
				   data, sizeof(data)) == 0);
	TEST_CHECK(result.sam_stat == SAM_STAT_GOOD);
	TEST_CHECK(result.data_len > 8);
	TEST_MSG("Read element status returned %u bytes", result.data_len);

	stop_daemon();
}

/* Test that minor 31 (second library tape) doesn't crash */
void test_second_library_tape(void)
{
	TEST_CHECK(start_daemon(31, "tape") == 0);
	clear_unit_attention();

	uint8_t cdb[6] = { 0x12, 0x00, 0x00, 0x00, 96, 0x00 };
	struct scsi_result result;
	uint8_t data[4096];

	TEST_CHECK(harness_send_cdb(&daemon_ctx, cdb, 6, &result,
				   data, sizeof(data)) == 0);
	TEST_CHECK(result.sam_stat == SAM_STAT_GOOD);
	TEST_CHECK(result.data_len >= 36);

	TEST_MSG("Product: %.16s", &data[16]);

	stop_daemon();
}

TEST_LIST = {
	{ "daemon_starts",              test_daemon_starts },
	{ "inquiry",                    test_inquiry },
	{ "test_unit_ready",            test_test_unit_ready },
	{ "mode_sense",                 test_mode_sense },
	{ "read_block_limits",          test_read_block_limits },
	{ "library_inquiry",            test_library_inquiry },
	{ "library_read_element_status",test_library_read_element_status },
	{ "second_library_tape",        test_second_library_tape },
	{ NULL, NULL }
};
