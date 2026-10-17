/*
 * Integration tests for device.conf header and Compression: parsing.
 *
 * sscanf() returns EOF when the input ends before the first conversion and a
 * partial count when only some fields match (ISO/IEC 9899:1999 7.19.6.7p3,
 * https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1256.pdf). vtltape and
 * vtllibrary used to treat any non-zero return as a full match, so:
 *  - an empty ' Compression:' line, or any whitespace-only line, in a drive
 *    stanza loaded uninitialised stack values into the compression settings;
 *  - a bare 'Drive: N' or 'Library: N' header (no CHANNEL/TARGET/LUN) copied
 *    an uninitialised struct mhvtl_ctl into the device address.
 *
 * Expected now: the malformed Compression lines leave the defaults alone, and
 * a header without its address is logged and the device is not configured,
 * so the daemon exits with "Can not find entry".
 *
 * Starts vtltape / vtllibrary with MHVTL_BACKEND=userland; no root needed.
 */
#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"

#define LIB_MINOR  20
#define TAPE_MINOR 21

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

/* Write a one-library, one-drive config. 'lib_hdr' / 'drive_hdr' are the
 * header lines; 'drive_extra' is appended to the drive stanza body.
 */
static int prepare_env(const char *lib_hdr, const char *drive_hdr,
		       const char *drive_extra)
{
	char path[1024];
	FILE *fp;

	memset(&test_env, 0, sizeof(test_env));
	snprintf(test_env.base_dir, sizeof(test_env.base_dir),
		 "/tmp/mhvtl-confscanf-XXXXXX");
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
		"%s\n"
		" Vendor identification: STK\n"
		" Product identification: L700\n"
		" Product revision level: 0108\n"
		" Unit serial number: TESTLIB00\n"
		" NAA: 50:11:22:33:44:55:66:00\n"
		" Home directory: %s\n"
		"\n"
		"%s\n"
		" Library ID: %u Slot: 1\n"
		" Vendor identification: IBM\n"
		" Product identification: ULT3580-TD8\n"
		" Product revision level: 0100\n"
		" Unit serial number: XYZZY_A\n"
		" NAA: 50:11:22:33:44:55:66:01\n"
		"%s"
		"\n",
		lib_hdr, test_env.home_dir, drive_hdr, LIB_MINOR, drive_extra);
	fclose(fp);

	snprintf(path, sizeof(path), "%s/library_contents.%u",
		 test_env.conf_dir, LIB_MINOR);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp, "Drive 1:\nPicker 1:\nMAP 1:\nSlot 1:\n");
	fclose(fp);

	return 0;
}

static char lib_hdr_full[64];
static char drive_hdr_full[64];

static void make_headers(void)
{
	snprintf(lib_hdr_full, sizeof(lib_hdr_full),
		 "Library: %u CHANNEL: 00 TARGET: 00 LUN: 00", LIB_MINOR);
	snprintf(drive_hdr_full, sizeof(drive_hdr_full),
		 "Drive: %u CHANNEL: 00 TARGET: 01 LUN: 00", TAPE_MINOR);
}

/* MODE SENSE(6), DBD set, current values: copy the page into 'pg' */
static int mode_sense_page(uint8_t page, uint8_t *pg, size_t len)
{
	struct scsi_result r;
	uint8_t cdb[6] = { 0x1a, 0x08, page, 0x00, 0xff, 0x00 };

	if (harness_cdb(&daemon_ctx, cdb, 6, &r) < 0 || r.sam_stat != 0)
		return -1;
	if (r.data_len < 4 + len)
		return -1;
	memcpy(pg, &r.data[4], len);
	return 0;
}

/* Start the tape daemon with 'drive_extra' in its stanza and read the Data
 * Compression (0x0F, 16 bytes) and Device Configuration (0x10, 16 bytes) pages.
 */
static int tape_compression_pages(const char *drive_extra, uint8_t *p0f,
				  uint8_t *p10)
{
	int rc = -1;

	if (prepare_env(lib_hdr_full, drive_hdr_full, drive_extra) < 0)
		goto out;
	if (harness_start_env(&daemon_ctx, TAPE_MINOR, "tape", &test_env) < 0)
		goto out;
	if (mode_sense_page(0x0f, p0f, 16) < 0)
		goto stop;
	if (mode_sense_page(0x10, p10, 16) < 0)
		goto stop;
	rc = 0;
stop:
	harness_stop(&daemon_ctx);
out:
	gen_cleanup_env(&test_env);
	return rc;
}

static void dump(const char *what, const uint8_t *p)
{
	char buf[64];
	int i, n = 0;

	for (i = 0; i < 16; i++)
		n += snprintf(buf + n, sizeof(buf) - n, "%02x ", p[i]);
	TEST_MSG("%s: %s", what, buf);
}

/* An empty ' Compression:' line and a whitespace-only line must leave the
 * compression settings at the defaults a stanza without them gets.
 */
static void test_malformed_compression_keeps_defaults(void)
{
	uint8_t base0f[16], base10[16], got0f[16], got10[16];

	make_headers();
	TEST_ASSERT(tape_compression_pages("", base0f, base10) == 0);
	TEST_ASSERT(tape_compression_pages(" Compression:\n"
					   "   \t \n",
					   got0f, got10) == 0);

	TEST_CHECK_(!(base0f[2] & 0x80), "default DCE is clear");
	TEST_CHECK_(memcmp(base0f, got0f, 16) == 0,
		    "page 0x0F unchanged by an empty Compression: line");
	dump("default 0x0F", base0f);
	dump("got     0x0F", got0f);
	TEST_CHECK_(memcmp(base10, got10, 16) == 0,
		    "page 0x10 unchanged by an empty Compression: line");
	dump("default 0x10", base10);
	dump("got     0x10", got10);

	/* 'factor N' without 'enabled M' is a partial match: ignored too */
	TEST_ASSERT(tape_compression_pages(" Compression: factor 9\n",
					   got0f, got10) == 0);
	TEST_CHECK_(memcmp(base0f, got0f, 16) == 0,
		    "page 0x0F unchanged by 'Compression: factor 9'");
	TEST_CHECK_(memcmp(base10, got10, 16) == 0,
		    "page 0x10 unchanged by 'Compression: factor 9'");
}

/* The form generate_device_conf writes still enables compression */
static void test_wellformed_compression_still_applies(void)
{
	uint8_t p0f[16], p10[16];

	make_headers();
	TEST_ASSERT(tape_compression_pages(" Compression: factor 1 enabled 1\n",
					   p0f, p10) == 0);
	TEST_CHECK_(p0f[2] & 0x80,
		    "DCE set by 'Compression: factor 1 enabled 1'");
	TEST_CHECK_(p10[14] == 1,
		    "page 0x10 SELECT DATA COMPRESSION ALGORITHM == 1 (got %u)",
		    p10[14]);
}

/* A drive header without CHANNEL/TARGET/LUN is rejected: no address is taken
 * from it, so vtltape finds no entry for its minor and exits.
 */
static void test_bare_drive_header_rejected(void)
{
	char hdr[32];

	make_headers();
	snprintf(hdr, sizeof(hdr), "Drive: %u", TAPE_MINOR);
	TEST_ASSERT(prepare_env(lib_hdr_full, hdr, "") == 0);
	TEST_CHECK_(harness_start_env(&daemon_ctx, TAPE_MINOR, "tape",
				      &test_env) < 0,
		    "vtltape refuses to start from a bare '%s' header", hdr);
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);

	/* Control: the same file with the full header starts */
	TEST_ASSERT(prepare_env(lib_hdr_full, drive_hdr_full, "") == 0);
	TEST_CHECK_(harness_start_env(&daemon_ctx, TAPE_MINOR, "tape",
				      &test_env) == 0,
		    "vtltape starts from '%s'", drive_hdr_full);
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

static void test_bare_library_header_rejected(void)
{
	char hdr[32];

	make_headers();
	snprintf(hdr, sizeof(hdr), "Library: %u", LIB_MINOR);
	TEST_ASSERT(prepare_env(hdr, drive_hdr_full, "") == 0);
	TEST_CHECK_(harness_start_env(&daemon_ctx, LIB_MINOR, "library",
				      &test_env) < 0,
		    "vtllibrary refuses to start from a bare '%s' header", hdr);
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);

	TEST_ASSERT(prepare_env(lib_hdr_full, drive_hdr_full, "") == 0);
	TEST_CHECK_(harness_start_env(&daemon_ctx, LIB_MINOR, "library",
				      &test_env) == 0,
		    "vtllibrary starts from '%s'", lib_hdr_full);
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

TEST_LIST = {
	{ "malformed_compression_keeps_defaults",
	  test_malformed_compression_keeps_defaults },
	{ "wellformed_compression_still_applies",
	  test_wellformed_compression_still_applies },
	{ "bare_drive_header_rejected", test_bare_drive_header_rejected },
	{ "bare_library_header_rejected", test_bare_library_header_rejected },
	{ NULL, NULL }
};
