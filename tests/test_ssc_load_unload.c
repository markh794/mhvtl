/*
 * Integration tests for LOAD/UNLOAD (1Bh) on an SSC drive.
 *
 * A LOAD with no cartridge in the drive is rejected with NOT READY, MEDIUM
 * NOT PRESENT, 2/3A/00 (IBM LTO SCSI Reference GA32-0928-07, 5.2.6
 * LOAD/UNLOAD, p.91). A cartridge that was unloaded but is still in the
 * drive is mounted again by a LOAD.
 *
 * While a host prevents medium removal, an unload by LOAD/UNLOAD or by the
 * operator is refused with ILLEGAL REQUEST, MEDIUM REMOVAL PREVENTED,
 * 5/53/02 (5.2.14.1, p.107, and Table 51, p.93), and LOG SENSE page 11h
 * reports PAMR (p.269).
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

static const struct drive_type *find_drive(const char *name)
{
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++)
		if (strcmp(dt->name, name) == 0)
			return dt;
	return NULL;
}

static int start_drive(const char *drive, int load)
{
	const struct drive_type *dt = find_drive(drive);
	struct scsi_result r;
	uint8_t tur[6] = { 0x00 };

	if (!dt || gen_test_env(&test_env, dt) < 0)
		return -1;
	if (harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env) < 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	if (!load)
		return 0;
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

static int load_unload(int load, struct scsi_result *r)
{
	uint8_t cdb[6] = { 0x1b, 0, 0, 0, (uint8_t)(load ? 0x01 : 0x00), 0 };

	return harness_cdb(&daemon_ctx, cdb, 6, r);
}

static int sense_is(const struct scsi_result *r, uint8_t key, uint8_t asc,
		    uint8_t ascq)
{
	return r->sam_stat == SAM_STAT_CHECK_CONDITION &&
	       (r->sense[2] & 0x0f) == key &&
	       r->sense[12] == asc && r->sense[13] == ascq;
}

static int request_sense(struct scsi_result *r, uint8_t *sense)
{
	uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };

	memset(sense, 0, 18);
	return harness_send_cdb(&daemon_ctx, cdb, 6, r, sense, 18);
}

#define CHECK_NOT_PRESENT(r, what) \
	TEST_CHECK_(sense_is(&(r), 0x02, 0x3a, 0x00), \
		    "%s: stat=%d sense %x/%02x/%02x (expected 2/3a/00)", what, \
		    (r).sam_stat, (r).sense[2] & 0x0f, (r).sense[12], (r).sense[13])

/* LOAD into an empty drive: NOT READY, MEDIUM NOT PRESENT; drive survives */
void test_load_no_medium(void)
{
	struct scsi_result r;
	uint8_t tur[6] = { 0x00 };

	TEST_ASSERT(start_drive("LTO-8", 0) == 0);

	load_unload(1, &r);
	CHECK_NOT_PRESENT(r, "LOAD with no cartridge");

	/* Same answer a second time, and the daemon is still serving commands */
	load_unload(1, &r);
	CHECK_NOT_PRESENT(r, "second LOAD with no cartridge");
	harness_cdb(&daemon_ctx, tur, 6, &r);
	CHECK_NOT_PRESENT(r, "TEST UNIT READY after LOAD");

	/* UNLOAD with no cartridge at all is also MEDIUM NOT PRESENT */
	load_unload(0, &r);
	CHECK_NOT_PRESENT(r, "UNLOAD with no cartridge");

	stop_drive();
}

/* UNLOAD leaves the cartridge in the drive; LOAD mounts it again */
void test_reload_after_unload(void)
{
	uint8_t b[BLK_SZ], rb[BLK_SZ];
	struct scsi_result r;
	uint8_t tur[6] = { 0x00 };
	uint8_t sense[18];

	TEST_ASSERT(start_drive("LTO-8", 1) == 0);

	memset(b, 0x3c, BLK_SZ);
	harness_tape_write(&daemon_ctx, b, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "write block");
	harness_tape_write_fm(&daemon_ctx, 1, &r);

	load_unload(0, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "UNLOAD stat=%d", r.sam_stat);
	harness_cdb(&daemon_ctx, tur, 6, &r);
	CHECK_NOT_PRESENT(r, "TEST UNIT READY after UNLOAD");

	/* The cartridge is present but already unloaded: UNLOAD is GOOD */
	load_unload(0, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "second UNLOAD stat=%d", r.sam_stat);

	load_unload(1, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
		    "reload LOAD stat=%d sense %x/%02x/%02x",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	request_sense(&r, sense);
	TEST_CHECK_((sense[2] & 0x0f) == 0 && sense[12] == 0 && sense[13] == 0,
		    "REQUEST SENSE after reload %x/%02x/%02x (expected 0/00/00)",
		    sense[2] & 0x0f, sense[12], sense[13]);
	harness_cdb(&daemon_ctx, tur, 6, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "TEST UNIT READY after reload");

	harness_tape_read(&daemon_ctx, rb, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && memcmp(rb, b, BLK_SZ) == 0,
		    "data written before UNLOAD reads back after reload");

	stop_drive();
}

static int edit_media_type(const char *barcode, const char *type)
{
	char edit_path[256];
	char cmd[1024];

	if (harness_find_binary("edit_tape", edit_path, sizeof(edit_path)) < 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "%sMHVTL_CONFIG_PATH=%s MHVTL_HOME_PATH=%s %s -l %d -m %s -t %s >/dev/null 2>&1",
		 gen_local_lib_prefix(edit_path),
		 test_env.conf_dir, test_env.home_dir, edit_path,
		 TEST_MINOR_LIBRARY, barcode, type);
	return system(cmd) == 0 ? 0 : -1;
}

/* A cartridge the drive cannot use fails the reload: INCOMPATIBLE MEDIUM */
void test_reload_incompatible_medium(void)
{
	const struct drive_type *dt = find_drive("LTO-1");
	struct scsi_result r;

	TEST_ASSERT(start_drive("LTO-1", 1) == 0);

	load_unload(0, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "UNLOAD stat=%d", r.sam_stat);

	/* LTO-1 has no WORM; swap the cartridge's medium type under it */
	TEST_ASSERT(edit_media_type(dt->media_barcode, "WORM") == 0);

	load_unload(1, &r);
	TEST_CHECK_(sense_is(&r, 0x03, 0x30, 0x00),
		    "reload of WORM media in LTO-1 stat=%d sense %x/%02x/%02x (expected 3/30/00)",
		    r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	stop_drive();
}

/* LOAD of an already loaded cartridge rewinds to BOT and stays GOOD */
void test_load_when_loaded(void)
{
	uint8_t b[BLK_SZ], rb[BLK_SZ];
	struct scsi_result r;

	TEST_ASSERT(start_drive("LTO-8", 1) == 0);

	memset(b, 0x77, BLK_SZ);
	harness_tape_write(&daemon_ctx, b, BLK_SZ, &r);
	harness_tape_write(&daemon_ctx, b, BLK_SZ, &r);

	load_unload(1, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "LOAD stat=%d", r.sam_stat);

	harness_tape_read(&daemon_ctx, rb, BLK_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && memcmp(rb, b, BLK_SZ) == 0,
		    "first block read after LOAD (positioned at BOT)");

	stop_drive();
}

/* PREVENT ALLOW MEDIUM REMOVAL with PREVENT = 01b (prevent) or 00b (allow) */
static void prevent(int on)
{
	uint8_t cdb[6] = { 0x1e, 0, 0, 0, (uint8_t)on, 0 };
	struct scsi_result r;

	harness_cdb(&daemon_ctx, cdb, 6, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "PREVENT ALLOW %d: stat=%d",
		    on, r.sam_stat);
}

/* LOG SENSE page 11h (Device Status): PAMR, byte 4 bit 7 of parameter 0000h */
static int pamr(void)
{
	uint8_t cdb[10] = { 0x4d, 0, 0x40 | 0x11, 0, 0, 0, 0, 0, 64, 0 };
	uint8_t data[64];
	struct scsi_result r;

	memset(data, 0, sizeof(data));
	harness_send_cdb(&daemon_ctx, cdb, 10, &r, data, sizeof(data));
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "LOG SENSE 11h: stat=%d",
		    r.sam_stat);
	return !!(data[8] & 0x80);
}

#define CHECK_PREVENTED(r, what) \
	TEST_CHECK_(sense_is(&(r), 0x05, 0x53, 0x02), \
		    "%s: stat=%d sense %x/%02x/%02x (expected 5/53/02)", what, \
		    (r).sam_stat, (r).sense[2] & 0x0f, (r).sense[12], (r).sense[13])

#define CHECK_GOOD(r, what) \
	TEST_CHECK_((r).sam_stat == SAM_STAT_GOOD, \
		    "%s: stat=%d sense %x/%02x/%02x (expected GOOD)", what, \
		    (r).sam_stat, (r).sense[2] & 0x0f, (r).sense[12], (r).sense[13])

/* Threaded: UNLOAD and HOLD are prevented; LOAD without HOLD is not */
void test_prevent_threaded(void)
{
	uint8_t tur[6] = { 0x00 };
	uint8_t unload_hold[6] = { 0x1b, 0, 0, 0, 0x08, 0 };
	uint8_t load_hold[6] = { 0x1b, 0, 0, 0, 0x09, 0 };
	struct scsi_result r;

	TEST_ASSERT(start_drive("LTO-8", 1) == 0);

	TEST_CHECK_(!pamr(), "PAMR set before PREVENT");
	prevent(1);
	TEST_CHECK_(pamr(), "PAMR clear after PREVENT");

	load_unload(0, &r);
	CHECK_PREVENTED(r, "UNLOAD while prevented");
	harness_cdb(&daemon_ctx, unload_hold, 6, &r);
	CHECK_PREVENTED(r, "UNLOAD with HOLD while prevented");
	harness_cdb(&daemon_ctx, load_hold, 6, &r);
	CHECK_PREVENTED(r, "LOAD with HOLD while prevented");
	harness_cdb(&daemon_ctx, tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY after the refused UNLOADs");
	load_unload(1, &r);
	CHECK_GOOD(r, "LOAD while prevented");

	prevent(0);
	TEST_CHECK_(!pamr(), "PAMR set after ALLOW");
	load_unload(0, &r);
	CHECK_GOOD(r, "UNLOAD after ALLOW");

	stop_drive();
}

/* Mounted but not threaded: UNLOAD (an eject) is prevented; HOLD and LOAD are not */
void test_prevent_mounted_not_threaded(void)
{
	uint8_t tur[6] = { 0x00 };
	uint8_t unload_hold[6] = { 0x1b, 0, 0, 0, 0x08, 0 };
	struct scsi_result r;

	TEST_ASSERT(start_drive("LTO-8", 1) == 0);

	load_unload(0, &r);
	CHECK_GOOD(r, "UNLOAD");
	prevent(1);
	load_unload(0, &r);
	CHECK_PREVENTED(r, "UNLOAD of an unloaded cartridge while prevented");
	harness_cdb(&daemon_ctx, unload_hold, 6, &r);
	CHECK_GOOD(r, "UNLOAD with HOLD of an unloaded cartridge while prevented");
	load_unload(1, &r);
	CHECK_GOOD(r, "LOAD while prevented");
	harness_cdb(&daemon_ctx, tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY after LOAD");

	stop_drive();
}

/* The operator's unload (vtlcmd) is refused while prevented */
void test_prevent_operator_unload(void)
{
	uint8_t tur[6] = { 0x00 };
	struct scsi_result r;
	char resp[256] = "";

	TEST_ASSERT(start_drive("LTO-8", 1) == 0);

	prevent(1);
	TEST_CHECK(harness_vtlcmd(TEST_MINOR_TAPE, "unload", resp, sizeof(resp)) == 0);
	TEST_CHECK_(!strncmp(resp, "Unload failed", 13),
		    "operator unload while prevented: reply '%s'", resp);
	harness_cdb(&daemon_ctx, tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY after the refused operator unload");

	prevent(0);
	memset(resp, 0, sizeof(resp));
	TEST_CHECK(harness_vtlcmd(TEST_MINOR_TAPE, "unload", resp, sizeof(resp)) == 0);
	TEST_CHECK_(!strncmp(resp, "Unloaded OK", 11),
		    "operator unload after ALLOW: reply '%s'", resp);
	harness_cdb(&daemon_ctx, tur, 6, &r);
	CHECK_NOT_PRESENT(r, "TEST UNIT READY after the operator unload");

	stop_drive();
}

TEST_LIST = {
	{ "load_no_medium",         test_load_no_medium },
	{ "reload_after_unload",    test_reload_after_unload },
	{ "reload_incompatible_medium", test_reload_incompatible_medium },
	{ "load_when_loaded",       test_load_when_loaded },
	{ "prevent_threaded",       test_prevent_threaded },
	{ "prevent_mounted_not_threaded", test_prevent_mounted_not_threaded },
	{ "prevent_operator_unload", test_prevent_operator_unload },
	{ NULL, NULL }
};
