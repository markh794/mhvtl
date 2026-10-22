/*
 * Integration tests for the unit attention a cartridge load leaves.
 *
 * Loading a cartridge (by the library, or with vtlcmd) establishes a unit
 * attention, NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED (06/28/00);
 * a cleaning cartridge reports CLEANING CARTRIDGE INSTALLED (06/30/03).
 * It is reported to the next command whose UAT column in Table 31 is 'Y',
 * which then does not run, and REQUEST SENSE returns it as sense data;
 * INQUIRY and LOG SENSE leave it pending (IBM LTO SCSI
 * Reference GA32-0928-07, 4.19.4 b), p.66, and Table 31, p.77-78).
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

static struct harness_daemon daemon_ctx;
static struct test_env test_env;
static const struct drive_type *drive;

static const uint8_t tur[6] = { 0x00 };

static int sense_is(const struct scsi_result *r, uint8_t key, uint8_t asc, uint8_t ascq)
{
	return r->sam_stat == SAM_STAT_CHECK_CONDITION &&
	       (r->sense[2] & 0x0f) == key &&
	       r->sense[12] == asc && r->sense[13] == ascq;
}

#define CHECK_SENSE(r, key, asc, ascq, what)                                      \
	TEST_CHECK_(sense_is(&(r), key, asc, ascq),                               \
		    "%s: sam_stat=%d sense %x/%02x/%02x, want %x/%02x/%02x", what, \
		    (r).sam_stat, (r).sense[2] & 0x0f, (r).sense[12], (r).sense[13],  \
		    key, asc, ascq)

#define CHECK_GOOD(r, what)                                                       \
	TEST_CHECK_((r).sam_stat == SAM_STAT_GOOD,                                \
		    "%s: sam_stat=%d sense %x/%02x/%02x, want GOOD", what,         \
		    (r).sam_stat, (r).sense[2] & 0x0f, (r).sense[12], (r).sense[13])

static void cdb(const uint8_t *c, int len, struct scsi_result *r)
{
	memset(r, 0, sizeof(*r));
	TEST_CHECK(harness_cdb(&daemon_ctx, c, len, r) == 0);
}

/* Recreate the default cartridge as type ("data", "WORM", "clean") and
 * density (NULL: the drive's own) */
static int remake_tape(const char *type, const char *density)
{
	char cmd[1024];
	char mktape_path[256];

	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;
	snprintf(cmd, sizeof(cmd), "rm -rf %s/%s", test_env.home_dir, drive->media_barcode);
	if (system(cmd) != 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "%s%s -C %s -H %s -l %d -m %s -s 500 -t %s -d %s >/dev/null 2>&1",
		 gen_local_lib_prefix(mktape_path), mktape_path,
		 test_env.conf_dir, test_env.home_dir, TEST_MINOR_LIBRARY,
		 drive->media_barcode, type, density ? density : drive->density);
	return system(cmd) == 0 ? 0 : -1;
}

/*
 * Start vtltape on an LTO-8 drive with the default cartridge made as type,
 * and take the power-on unit attention, as a host would.
 */
static int start_drive(const char *type, const char *density)
{
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++)
		if (!strcmp(dt->name, "LTO-8"))
			drive = dt;
	if (!drive || gen_test_env(&test_env, drive) < 0)
		return -1;
	if ((strcmp(type, "data") || density) && remake_tape(type, density) < 0)
		return -1;
	if (harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env) < 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	return 0;
}

static void stop_drive(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

/*
 * Load the cartridge without sending a command that would take the unit
 * attention: vtltape reads its queue between commands and at least once a
 * second when idle.
 */
static void load_quietly(void)
{
	TEST_ASSERT(harness_load_tape(TEST_MINOR_TAPE, drive->media_barcode) == 0);
	sleep(2);
}

static void test_mode_sense_first(void)
{
	uint8_t ms[6] = { 0x1a, 0x00, 0x3f, 0x00, 0xff, 0x00 };
	struct scsi_result r;

	TEST_ASSERT(start_drive("data", NULL) == 0);
	load_quietly();
	cdb(ms, 6, &r);
	CHECK_SENSE(r, UNIT_ATTENTION, 0x28, 0x00, "MODE SENSE after the load");
	cdb(ms, 6, &r);
	CHECK_GOOD(r, "MODE SENSE again");
	cdb(tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY after the unit attention was reported");
	stop_drive();
}

static void test_report_density_first(void)
{
	uint8_t rds[10] = { 0x44, 0x01, 0, 0, 0, 0, 0, 0x04, 0x00, 0 };
	struct scsi_result r;

	TEST_ASSERT(start_drive("data", NULL) == 0);
	load_quietly();
	cdb(rds, 10, &r);
	CHECK_SENSE(r, UNIT_ATTENTION, 0x28, 0x00, "REPORT DENSITY SUPPORT after the load");
	cdb(rds, 10, &r);
	CHECK_GOOD(r, "REPORT DENSITY SUPPORT again");
	stop_drive();
}

static void test_tur_first(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_drive("data", NULL) == 0);
	load_quietly();
	cdb(tur, 6, &r);
	CHECK_SENSE(r, UNIT_ATTENTION, 0x28, 0x00, "TEST UNIT READY after the load");
	cdb(tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY again");
	stop_drive();
}

/* A write that the unit attention ends must not reach the tape */
static void test_write_first_not_run(void)
{
	uint8_t wr[6] = { 0x0a, 0x00, 0x00, 0x02, 0x00, 0x00 };	/* 512 bytes, variable */
	uint8_t rp[10] = { 0x34, 0x00 };			/* READ POSITION, short form */
	uint8_t blk[512];
	struct scsi_result r;

	memset(blk, 0xa5, sizeof(blk));
	TEST_ASSERT(start_drive("data", NULL) == 0);
	load_quietly();
	memset(&r, 0, sizeof(r));
	TEST_CHECK(harness_send_cdb_with_data_out(&daemon_ctx, wr, 6, blk, sizeof(blk), &r) >= 0);
	CHECK_SENSE(r, UNIT_ATTENTION, 0x28, 0x00, "WRITE(6) after the load");
	cdb(rp, 10, &r);
	CHECK_GOOD(r, "READ POSITION");
	TEST_CHECK_(r.data_len >= 8 && !r.data[4] && !r.data[5] && !r.data[6] && !r.data[7],
		    "still at block 0: the write did not run");
	stop_drive();
}

static void test_exempt_commands_leave_it_pending(void)
{
	uint8_t inq[6] = { 0x12, 0x00, 0x00, 0x00, 0x60, 0x00 };
	uint8_t ls[10] = { 0x4d, 0x00, 0x40, 0, 0, 0, 0, 0x01, 0x00, 0 };	/* supported pages */
	struct scsi_result r;

	TEST_ASSERT(start_drive("data", NULL) == 0);
	load_quietly();
	cdb(inq, 6, &r);
	CHECK_GOOD(r, "INQUIRY after the load");
	cdb(ls, 10, &r);
	CHECK_GOOD(r, "LOG SENSE after the load");
	cdb(tur, 6, &r);
	CHECK_SENSE(r, UNIT_ATTENTION, 0x28, 0x00, "TEST UNIT READY after INQUIRY and LOG SENSE");
	stop_drive();
}

static void test_request_sense_reports_it(void)
{
	uint8_t rs[6] = { 0x03, 0x00, 0x00, 0x00, 0x12, 0x00 };
	struct scsi_result r;

	TEST_ASSERT(start_drive("data", NULL) == 0);
	load_quietly();
	cdb(rs, 6, &r);
	CHECK_GOOD(r, "REQUEST SENSE after the load");
	TEST_CHECK_(r.data_len >= 14 && (r.data[2] & 0x0f) == UNIT_ATTENTION &&
		    r.data[12] == 0x28 && r.data[13] == 0x00,
		    "REQUEST SENSE data %x/%02x/%02x, want 6/28/00",
		    r.data_len >= 3 ? r.data[2] & 0x0f : 0xff,
		    r.data_len >= 13 ? r.data[12] : 0xff, r.data_len >= 14 ? r.data[13] : 0xff);
	cdb(tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY after REQUEST SENSE");
	stop_drive();
}

static void test_worm_load(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_drive("WORM", NULL) == 0);
	load_quietly();
	cdb(tur, 6, &r);
	CHECK_SENSE(r, UNIT_ATTENTION, 0x28, 0x00, "TEST UNIT READY after a WORM load");
	cdb(tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY again");
	stop_drive();
}

static void test_cleaning_load(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_drive("clean", NULL) == 0);
	load_quietly();
	cdb(tur, 6, &r);
	CHECK_SENSE(r, UNIT_ATTENTION, 0x30, 0x03, "TEST UNIT READY after a cleaning cartridge load");
	stop_drive();
}

static void test_unload_clears_it(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_drive("data", NULL) == 0);
	load_quietly();
	TEST_CHECK(harness_unload_tape(TEST_MINOR_TAPE) == 0);
	cdb(tur, 6, &r);
	CHECK_SENSE(r, NOT_READY, 0x3a, 0x00, "TEST UNIT READY after load and unload");
	stop_drive();
}

/* A cartridge the drive refuses leaves no unit attention behind */
static void test_failed_load_posts_none(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_drive("data", "LTO3") == 0);	/* an LTO-8 drive does not load LTO-3 */
	load_quietly();
	cdb(tur, 6, &r);
	CHECK_SENSE(r, NOT_READY, 0x3a, 0x00, "TEST UNIT READY after a refused load");
	stop_drive();
}

/* The initiator that sends LOAD gets no unit attention for it */
static void test_host_load_posts_none(void)
{
	uint8_t unload[6] = { 0x1b, 0x00, 0x00, 0x00, 0x00, 0x00 };
	uint8_t load[6] = { 0x1b, 0x00, 0x00, 0x00, 0x01, 0x00 };
	struct scsi_result r;

	TEST_ASSERT(start_drive("data", NULL) == 0);
	load_quietly();
	cdb(tur, 6, &r);
	CHECK_SENSE(r, UNIT_ATTENTION, 0x28, 0x00, "TEST UNIT READY after the load");
	cdb(unload, 6, &r);
	CHECK_GOOD(r, "UNLOAD");
	cdb(load, 6, &r);
	CHECK_GOOD(r, "LOAD");
	cdb(tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY after the host's LOAD");
	stop_drive();
}

TEST_LIST = {
	{ "mode_sense_first", test_mode_sense_first },
	{ "report_density_first", test_report_density_first },
	{ "tur_first", test_tur_first },
	{ "write_first_not_run", test_write_first_not_run },
	{ "exempt_commands_leave_it_pending", test_exempt_commands_leave_it_pending },
	{ "request_sense_reports_it", test_request_sense_reports_it },
	{ "worm_load", test_worm_load },
	{ "cleaning_load", test_cleaning_load },
	{ "unload_clears_it", test_unload_clears_it },
	{ "failed_load_posts_none", test_failed_load_posts_none },
	{ "host_load_posts_none", test_host_load_posts_none },
	{ NULL, NULL }
};
