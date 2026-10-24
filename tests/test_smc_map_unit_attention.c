/*
 * Integration tests for the unit attention an operator's access to the
 * import/export element (MAP) leaves on the library.
 *
 * When the operator opens or closes the MAP (vtlcmd "open map", "close map",
 * as mhvtl-bay-monitor does around "load map" and "empty map"), the library
 * reports IMPORT OR EXPORT ELEMENT ACCESSED (06/28/01) to the next command,
 * which then does not run. REQUEST SENSE returns it as sense data, and
 * INQUIRY leaves it pending. A state change of the import/export element
 * establishes it for every initiator other than the one whose OPEN/CLOSE
 * IMPORT/EXPORT ELEMENT command caused it (T10/05-070r3, 5.3.4); the sense
 * code is in IBM TS2900 SCSI Reference GC27-2211-00, Table 68, p.58.
 *
 * Runs vtllibrary with MHVTL_BACKEND=userland as an IBM 03584; no kernel
 * module or root needed.
 */
#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"

#define LIB_MINOR  10
#define TAPE_MINOR 11
#define MAP_ADDR   0x0301 /* first import/export element of the 03584 */

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

static const uint8_t tur[6] = { 0x00 };

static int prepare_env(void)
{
	char path[1024];
	FILE *fp;

	memset(&test_env, 0, sizeof(test_env));
	snprintf(test_env.base_dir, sizeof(test_env.base_dir),
		 "/tmp/mhvtl-mapua-XXXXXX");
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
		" Vendor identification: IBM\n"
		" Product identification: 03584L22\n"
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
		LIB_MINOR, test_env.home_dir, TAPE_MINOR, LIB_MINOR);
	fclose(fp);

	snprintf(path, sizeof(path), "%s/library_contents.%u",
		 test_env.conf_dir, LIB_MINOR);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp, "Drive 1:\nPicker 1:\nMAP 1:\nMAP 2:\nSlot 1:\nSlot 2:\n");
	fclose(fp);
	return 0;
}

/* Start vtllibrary and take the power-on unit attention, as a host would */
static int start_library(void)
{
	if (prepare_env() != 0)
		return -1;
	if (harness_start_env(&daemon_ctx, LIB_MINOR, "library", &test_env) != 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	return 0;
}

static void stop_library(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

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

#define CHECK_ACCESSED(r, what) CHECK_SENSE(r, UNIT_ATTENTION, 0x28, 0x01, what)

static void cdb(const uint8_t *c, int len, struct scsi_result *r)
{
	memset(r, 0, sizeof(*r));
	TEST_CHECK(harness_cdb(&daemon_ctx, c, len, r) == 0);
}

/* Send an operator command to the library and check it answers OK */
static void operator(const char *what)
{
	char reply[256] = "";

	TEST_CHECK_(harness_vtlcmd(LIB_MINOR, what, reply, sizeof(reply)) == 0 &&
			    !strncmp(reply, "OK", 2),
		    "%s: reply '%s'", what, reply);
}

/* OPEN/CLOSE IMPORT/EXPORT ELEMENT (1Bh) on the first MAP element */
static void host_open_close(int close, struct scsi_result *r)
{
	uint8_t c[6] = { 0x1b, 0, 0, 0, (uint8_t)close, 0 };

	put_unaligned_be16(MAP_ADDR, &c[2]);
	cdb(c, 6, r);
}

/* The operator puts a cartridge in the MAP, as mhvtl-bay-monitor does */
static void test_import(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	operator("open map");
	operator("load map E01001L8");
	operator("close map");

	cdb(tur, 6, &r);
	CHECK_ACCESSED(r, "TEST UNIT READY after the import");
	/* One unit attention for the whole access, not one per open and close */
	cdb(tur, 6, &r);
	CHECK_GOOD(r, "second TEST UNIT READY");
	stop_library();
}

static void test_open_alone(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	operator("open map");
	cdb(tur, 6, &r);
	CHECK_ACCESSED(r, "TEST UNIT READY after open map");
	cdb(tur, 6, &r);
	CHECK_GOOD(r, "second TEST UNIT READY");
	stop_library();
}

/* Closing a closed MAP changes nothing */
static void test_no_change_posts_none(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	operator("close map");
	cdb(tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY after closing a closed MAP");
	stop_library();
}

/* The command the unit attention ends does not run */
static void test_command_not_run(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	operator("open map");
	host_open_close(1, &r);
	CHECK_ACCESSED(r, "host CLOSE IMPORT/EXPORT ELEMENT after open map");
	/* The close did not run: the MAP is still open, so empty map works */
	operator("empty map");
	stop_library();
}

static void test_inquiry_leaves_it_pending(void)
{
	struct scsi_result r;
	uint8_t inq[6] = { 0x12, 0, 0, 0, 0x60, 0 };

	TEST_ASSERT(start_library() == 0);
	operator("open map");
	cdb(inq, 6, &r);
	CHECK_GOOD(r, "INQUIRY after open map");
	cdb(tur, 6, &r);
	CHECK_ACCESSED(r, "TEST UNIT READY after INQUIRY");
	stop_library();
}

static void test_request_sense_reports_it(void)
{
	struct scsi_result r;
	uint8_t rs[6] = { 0x03, 0, 0, 0, 18, 0 };
	uint8_t data[18];

	TEST_ASSERT(start_library() == 0);
	operator("open map");
	memset(data, 0, sizeof(data));
	memset(&r, 0, sizeof(r));
	TEST_CHECK(harness_send_cdb(&daemon_ctx, rs, 6, &r, data, sizeof(data)) == 0);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "REQUEST SENSE stat=%d", r.sam_stat);
	TEST_CHECK_((data[2] & 0x0f) == UNIT_ATTENTION && data[12] == 0x28 &&
			    data[13] == 0x01,
		    "REQUEST SENSE data %x/%02x/%02x, want 6/28/01",
		    data[2] & 0x0f, data[12], data[13]);
	cdb(tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY after REQUEST SENSE");
	stop_library();
}

/* The initiator that opens or closes the MAP gets no unit attention for it */
static void test_host_open_close_posts_none(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	host_open_close(0, &r);
	CHECK_GOOD(r, "host OPEN IMPORT/EXPORT ELEMENT");
	host_open_close(1, &r);
	CHECK_GOOD(r, "host CLOSE IMPORT/EXPORT ELEMENT");
	cdb(tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY after the host opened and closed the MAP");
	stop_library();
}

TEST_LIST = {
	{ "import", test_import },
	{ "open_alone", test_open_alone },
	{ "no_change_posts_none", test_no_change_posts_none },
	{ "command_not_run", test_command_not_run },
	{ "inquiry_leaves_it_pending", test_inquiry_leaves_it_pending },
	{ "request_sense_reports_it", test_request_sense_reports_it },
	{ "host_open_close_posts_none", test_host_open_close_posts_none },
	{ NULL, NULL }
};
