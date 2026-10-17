/*
 * Integration tests for the sense a MOVE MEDIUM returns when the drive
 * fails to load the cartridge - markh794/mhvtl#278.
 *
 * Starts vtltape and vtllibrary with MHVTL_BACKEND=userland against a
 * generated device.conf, moves cartridges from storage slots into the drive
 * and decodes the sense data.
 *
 * GC27-2211-00 (IBM TS2900 SCSI Reference) Table 68, p.58:
 *   05/30/00 INCOMPATIBLE MEDIUM INSTALLED
 *   04/53/00 MEDIA LOAD OR EJECT FAILED
 */
#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"

#define LIB_MINOR  10
#define TAPE_MINOR 11

#define BC_LTO3	   "E01001L3" /* LTO-3 media: an LTO-8 drive cannot use it */
#define BC_MISSING "E01002L8" /* listed in the library, never created */
#define BC_LTO8	   "E01003L8"

static struct harness_daemon lib_ctx;
static struct harness_daemon tape_ctx;
static struct test_env test_env;

struct lib_conf {
	const char *vendor;
	const char *product;
	uint16_t drive;	  /* first data transfer element address */
	uint16_t storage; /* first storage element address */
};

static const struct lib_conf libs[] = {
	{ "IBM", "03584L32", 0x0101, 0x0400 },
	{ "STK", "L700",     0x01f4, 0x03e8 },
};

static int mktape(const char *barcode, const char *density)
{
	char mktape_path[256];
	char cmd[1024];

	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "%s%s -C %s -H %s -l %u -m %s -s 500 -t data -d %s >/dev/null 2>&1",
		 gen_local_lib_prefix(mktape_path), mktape_path,
		 test_env.conf_dir, test_env.home_dir, LIB_MINOR, barcode,
		 density);
	return system(cmd);
}

static int prepare_env(const struct lib_conf *c)
{
	char path[1024];
	FILE *fp;

	memset(&test_env, 0, sizeof(test_env));
	snprintf(test_env.base_dir, sizeof(test_env.base_dir),
		 "/tmp/mhvtl-loadfail-XXXXXX");
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
		" Vendor identification: %s\n"
		" Product identification: %s\n"
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
		LIB_MINOR, c->vendor, c->product, test_env.home_dir,
		TAPE_MINOR, LIB_MINOR);
	fclose(fp);

	snprintf(path, sizeof(path), "%s/library_contents.%u",
		 test_env.conf_dir, LIB_MINOR);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp, "Drive 1:\nPicker 1:\nMAP 1:\n"
		"Slot 1: %s\nSlot 2: %s\nSlot 3: %s\n",
		BC_LTO3, BC_MISSING, BC_LTO8);
	fclose(fp);

	if (mktape(BC_LTO3, "LTO3") != 0)
		return -1;
	return mktape(BC_LTO8, "LTO8");
}

/* The drive first, so it is there to answer the library */
static int start(const struct lib_conf *c)
{
	gen_cleanup_env(&test_env);
	if (prepare_env(c) != 0)
		return -1;
	if (harness_start_env(&tape_ctx, TAPE_MINOR, "tape", &test_env) != 0)
		return -1;
	if (harness_start_env(&lib_ctx, LIB_MINOR, "library", &test_env) != 0)
		return -1;
	harness_clear_ua(&lib_ctx);
	return 0;
}

static void stop(void)
{
	harness_stop(&lib_ctx);
	harness_stop(&tape_ctx);
	gen_cleanup_env(&test_env);
}

static int move_medium(uint16_t src, uint16_t dest, struct scsi_result *r)
{
	uint8_t cdb[12] = {
		0xa5, 0x00,
		0x00, 0x01, /* transport element */
		src >> 8, src & 0xff,
		dest >> 8, dest & 0xff,
		0x00, 0x00, 0x00, 0x00
	};

	return harness_cdb(&lib_ctx, cdb, 12, r);
}

/* 1 if the element at 'addr' is full, 0 if empty, -1 on error */
static int element_full(uint16_t addr)
{
	uint8_t cdb[12] = {
		0xb8, 0x00, /* all element types */
		addr >> 8, addr & 0xff,
		0x00, 0x01, /* one element */
		0x00,
		0x00, 0x04, 0x00, /* allocation length 1024 */
		0x00, 0x00
	};
	uint8_t buf[1024];
	struct scsi_result r;

	memset(buf, 0, sizeof(buf));
	if (harness_send_cdb(&lib_ctx, cdb, 12, &r, buf, sizeof(buf)) != 0)
		return -1;
	if (r.sam_stat != SAM_STAT_GOOD || r.data_len < 16 + 3)
		return -1;
	/* Header 8, page header 8, then the descriptor */
	if (get_unaligned_be16(&buf[16]) != addr)
		return -1;
	return buf[16 + 2] & 0x01;
}

static void check_sense(const struct scsi_result *r, uint8_t key,
			uint8_t asc, uint8_t ascq)
{
	TEST_CHECK(r->sam_stat == SAM_STAT_CHECK_CONDITION);
	TEST_CHECK((r->sense[2] & 0x0f) == key);
	TEST_CHECK(r->sense[12] == asc);
	TEST_CHECK(r->sense[13] == ascq);
	TEST_MSG("sam_stat 0x%02x, sense %02x/%02x/%02x, expected %02x/%02x/%02x",
		 r->sam_stat, r->sense[2] & 0x0f, r->sense[12], r->sense[13],
		 key, asc, ascq);
}

void test_incompatible_medium(void)
{
	for (unsigned i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
		const struct lib_conf *c = &libs[i];
		struct scsi_result r;

		TEST_CASE(c->product);
		TEST_ASSERT(start(c) == 0);

		TEST_ASSERT(move_medium(c->storage, c->drive, &r) == 0);
		check_sense(&r, ILLEGAL_REQUEST, 0x30, 0x00);
		/* The cartridge stays in its source slot */
		TEST_CHECK(element_full(c->storage) == 1);
		TEST_CHECK(element_full(c->drive) == 0);

		stop();
	}
}

void test_load_failed(void)
{
	for (unsigned i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
		const struct lib_conf *c = &libs[i];
		struct scsi_result r;

		TEST_CASE(c->product);
		TEST_ASSERT(start(c) == 0);

		/* The drive cannot open the cartridge: not a media type issue */
		TEST_ASSERT(move_medium(c->storage + 1, c->drive, &r) == 0);
		check_sense(&r, HARDWARE_ERROR, 0x53, 0x00);
		TEST_CHECK(element_full(c->storage + 1) == 1);
		TEST_CHECK(element_full(c->drive) == 0);

		/* The drive still loads a cartridge it can use */
		TEST_ASSERT(move_medium(c->storage + 2, c->drive, &r) == 0);
		TEST_CHECK(r.sam_stat == SAM_STAT_GOOD);
		TEST_MSG("sam_stat 0x%02x, sense %02x/%02x/%02x", r.sam_stat,
			 r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
		TEST_CHECK(element_full(c->storage + 2) == 0);
		TEST_CHECK(element_full(c->drive) == 1);

		stop();
	}
}

TEST_LIST = {
	{ "incompatible_medium", test_incompatible_medium },
	{ "load_failed",         test_load_failed },
	{ NULL, NULL }
};
