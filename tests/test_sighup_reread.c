/*
 * Characterization test for SIGHUP config re-read in vtllibrary
 * (markh794/mhvtl#272, step 1 - lock today's behaviour before the event-loop
 * rework).
 *
 * SIGHUP only raises reread_config_requested; do_rereadconfig() runs from the
 * main loop at a safe point between commands, re-reads device.conf and
 * library_contents, and calls reset_device() so the next command reports a
 * POWER-ON/RESET unit attention. This test starts vtllibrary, puts a cartridge
 * into a previously empty storage slot by rewriting library_contents on disk,
 * sends SIGHUP, and checks that the re-read happened: the next command returns
 * the POWER-ON/RESET unit attention, and READ ELEMENT STATUS then shows the
 * slot full.
 *
 * Starts the daemon with MHVTL_BACKEND=userland against a generated
 * device.conf, like tests/test_smc_dvcid.c.
 */
#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"

#include <signal.h>

#define LIB_MINOR 10

#define BARCODE1 "SGH001L8"
#define BARCODE2 "SGH002L8"

static struct harness_daemon daemon_ctx;
static struct test_env test_env;
static uint8_t res_buf[16384];

/* Write library_contents.LIB_MINOR; slot2_barcode NULL leaves Slot 2 empty. */
static int write_library_contents(const char *slot2_barcode)
{
	char  path[1024];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/library_contents.%u",
		 test_env.conf_dir, LIB_MINOR);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp,
		"Drive 1:\n"
		"Picker 1:\n"
		"MAP 1:\n"
		"Slot 1: %s\n"
		"Slot 2:%s%s\n",
		BARCODE1,
		slot2_barcode ? " " : "", slot2_barcode ? slot2_barcode : "");
	fclose(fp);
	return 0;
}

static int mktape_barcode(const char *barcode)
{
	char path[1024], mktape_path[256];

	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;
	snprintf(path, sizeof(path),
		 "%s%s -C %s -H %s -l %u -m %s -s 500 -t data -d LTO8 >/dev/null 2>&1",
		 gen_local_lib_prefix(mktape_path), mktape_path,
		 test_env.conf_dir, test_env.home_dir, LIB_MINOR, barcode);
	return system(path);
}

static int prepare_env(void)
{
	char  path[1024];
	FILE *fp;

	memset(&test_env, 0, sizeof(test_env));
	snprintf(test_env.base_dir, sizeof(test_env.base_dir),
		 "/tmp/mhvtl-sighup-XXXXXX");
	if (!mkdtemp(test_env.base_dir))
		return -1;
	snprintf(test_env.conf_dir, sizeof(test_env.conf_dir), "%s/conf", test_env.base_dir);
	snprintf(test_env.home_dir, sizeof(test_env.home_dir), "%s/home", test_env.base_dir);
	snprintf(test_env.lock_dir, sizeof(test_env.lock_dir), "%s/lock", test_env.base_dir);
	snprintf(test_env.run_dir,  sizeof(test_env.run_dir),  "%s/run",  test_env.base_dir);
	mkdir(test_env.conf_dir, 0755);
	mkdir(test_env.home_dir, 0755);
	mkdir(test_env.lock_dir, 0755);
	mkdir(test_env.run_dir, 0755);

	snprintf(path, sizeof(path), "%s/mhvtl.conf", test_env.conf_dir);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp, "MHVTL_CONFIG_PATH=%s\nMHVTL_HOME_PATH=%s\n",
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
		" Product revision level: 0100\n"
		" Unit serial number: TESTLIB00\n"
		" NAA: 50:11:22:33:44:55:66:00\n"
		" Home directory: %s\n"
		"\n"
		"Drive: %u CHANNEL: 00 TARGET: 01 LUN: 00\n"
		" Library ID: %u Slot: 1\n"
		" Vendor identification: IBM\n"
		" Product identification: ULT3580-TD8\n"
		" Product revision level: 0100\n"
		" Unit serial number: XYZZY_A\n"
		" NAA: 50:11:22:33:44:55:66:01\n"
		"\n",
		LIB_MINOR, test_env.home_dir, LIB_MINOR + 1, LIB_MINOR);
	fclose(fp);

	/* Both cartridges' media exist up front; only library_contents changes. */
	if (mktape_barcode(BARCODE1) != 0 || mktape_barcode(BARCODE2) != 0)
		return -1;
	return write_library_contents(NULL); /* Slot 2 starts empty */
}

/* FULL bit of the Nth storage element (0-based). -1 if not found/parseable. */
static int storage_slot_full(int nth)
{
	uint8_t cdb[12] = {
		0xb8, 0x02,		  /* READ ELEMENT STATUS, element type 2 = storage */
		0x00, 0x00, 0xff, 0xff,
		0x00, 0x00, 0x40, 0x00,	  /* allocation length 16384 */
		0x00, 0x00
	};
	struct scsi_result r;
	int total, dl, bc;
	uint8_t *p, *end, *d;

	memset(res_buf, 0, sizeof(res_buf));
	if (harness_send_cdb(&daemon_ctx, cdb, 12, &r, res_buf, sizeof(res_buf)) != 0)
		return -1;
	if (r.sam_stat != SAM_STAT_GOOD)
		return -1;
	if (r.data_len < 8)
		return -1;
	total = get_unaligned_be24(&res_buf[5]);
	p = &res_buf[8];
	end = p + total;
	while (p + 8 <= end) {
		dl = get_unaligned_be16(&p[2]);
		bc = get_unaligned_be24(&p[5]);
		if (p[0] == 0x02) {		  /* storage element page */
			d = p + 8 + nth * dl;
			if (d + 3 > end || dl < 3)
				return -1;
			return d[2] & 0x01;	  /* FULL bit */
		}
		p += 8 + bc;
	}
	return -1;
}

/* TEST UNIT READY; returns sam_stat, fills sense. */
static int tur(struct scsi_result *r)
{
	uint8_t cdb[6] = { 0x00 };
	harness_cdb(&daemon_ctx, cdb, 6, r);
	return r->sam_stat;
}

void test_sighup_rereads_library_contents(void)
{
	struct scsi_result r;
	int i, full, saw_ua = 0;

	TEST_ASSERT(prepare_env() == 0);
	TEST_ASSERT(harness_start_env(&daemon_ctx, LIB_MINOR, "library", &test_env) == 0);
	harness_clear_ua(&daemon_ctx);

	/* Baseline: Slot 1 full, Slot 2 empty. */
	TEST_CHECK_(storage_slot_full(0) == 1, "Slot 1 full at start");
	TEST_CHECK_(storage_slot_full(1) == 0, "Slot 2 empty at start");

	/* Put a cartridge in Slot 2 on disk and ask the daemon to re-read. */
	TEST_ASSERT(write_library_contents(BARCODE2) == 0);
	TEST_ASSERT(daemon_ctx.pid > 0);
	TEST_ASSERT(kill(daemon_ctx.pid, SIGHUP) == 0);

	/*
	 * do_rereadconfig() runs at a safe point in the loop and calls
	 * reset_device(), so a command after the re-read returns a POWER-ON/
	 * RESET unit attention (sense key 6, ASC 0x29). Poll (not a fixed
	 * sleep) until we see it, up to ~5 s.
	 */
	for (i = 0; i < 100; i++) {
		if (tur(&r) == SAM_STAT_CHECK_CONDITION &&
		    (r.sense[2] & 0x0f) == 0x06 && r.sense[12] == 0x29) {
			saw_ua = 1;
			break;
		}
		usleep(50000);
	}
	TEST_CHECK_(saw_ua, "POWER-ON/RESET unit attention after SIGHUP re-read");

	/* The UA is consumed; Slot 2 now reports full from the re-read config. */
	full = storage_slot_full(1);
	TEST_CHECK_(full == 1, "Slot 2 full after SIGHUP re-read (got %d)", full);
	TEST_CHECK_(storage_slot_full(0) == 1, "Slot 1 still full after re-read");

	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

TEST_LIST = {
	{ "sighup_rereads_library_contents", test_sighup_rereads_library_contents },
	{ NULL, NULL }
};
