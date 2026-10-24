/*
 * Integration tests for PREVENT ALLOW MEDIUM REMOVAL on the library.
 *
 * While a host prevents medium removal the library inhibits removal of
 * cartridges by an operator (IBM TS2900 SCSI Reference GC27-2211-00, p.41):
 * vtllibrary refuses the operator's "open map" (vtlcmd, as mhvtl-bay-monitor
 * sends it) and, if the MAP was already open, "empty map". Putting a
 * cartridge into an open MAP, and the host's own MOVE MEDIUM and OPEN/CLOSE
 * IMPORT/EXPORT ELEMENT, still work.
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
#define SLOT_ADDR  0x0400 /* first storage element of the 03584 */

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

static const uint8_t tur[6] = { 0x00 };

static int prepare_env(void)
{
	char path[1024];
	FILE *fp;

	memset(&test_env, 0, sizeof(test_env));
	snprintf(test_env.base_dir, sizeof(test_env.base_dir),
		 "/tmp/mhvtl-mapprev-XXXXXX");
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

static void cdb(const uint8_t *c, int len, struct scsi_result *r)
{
	memset(r, 0, sizeof(*r));
	TEST_CHECK(harness_cdb(&daemon_ctx, c, len, r) == 0);
}

/* Send an operator command to the library and check its answer */
static void operator_gets(const char *what, const char *want)
{
	char reply[256] = "";

	TEST_CHECK_(harness_vtlcmd(LIB_MINOR, what, reply, sizeof(reply)) == 0 &&
			    !strncmp(reply, want, strlen(want)),
		    "%s: reply '%s', want '%s'", what, reply, want);
}

static void operator(const char *what)
{
	operator_gets(what, "OK");
}

static void prevent(int on, const char *what)
{
	uint8_t c[6] = { 0x1e, 0, 0, 0, (uint8_t)on, 0 };
	struct scsi_result r;

	cdb(c, 6, &r);
	CHECK_GOOD(r, what);
}

/* OPEN/CLOSE IMPORT/EXPORT ELEMENT (1Bh) on the first MAP element */
static void host_open_close(int close, struct scsi_result *r)
{
	uint8_t c[6] = { 0x1b, 0, 0, 0, (uint8_t)close, 0 };

	put_unaligned_be16(MAP_ADDR, &c[2]);
	cdb(c, 6, r);
}

/* The operator cannot open the MAP, so nothing changes for the host */
static void test_open_refused(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	prevent(1, "PREVENT");
	operator_gets("open map", "MAP locked - medium removal prevented");
	cdb(tur, 6, &r);
	CHECK_GOOD(r, "TEST UNIT READY after the refused open map");
	/* Still closed: load map needs an open MAP */
	operator_gets("load map E01001L8", "MAP not opened");

	prevent(0, "ALLOW");
	operator("open map");
	stop_library();
}

/* The MAP was open when the host prevented removal */
static void test_empty_refused_while_open(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	operator("open map");
	cdb(tur, 6, &r); /* takes the unit attention of the open */
	operator("load map E01001L8");
	prevent(1, "PREVENT");
	operator_gets("empty map E01001L8", "Can't empty map - medium removal prevented");
	operator_gets("empty map", "Can't empty map - medium removal prevented");
	/* Putting a cartridge in is not removal */
	operator("load map E01002L8");
	operator("close map");
	cdb(tur, 6, &r); /* takes the unit attention of the close */

	prevent(0, "ALLOW");
	operator("open map");
	operator("empty map E01001L8");
	stop_library();
}

/* The host controls removal; its own OPEN/CLOSE IMPORT/EXPORT ELEMENT works */
static void test_host_open_close_allowed(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	prevent(1, "PREVENT");
	host_open_close(0, &r);
	CHECK_GOOD(r, "host OPEN IMPORT/EXPORT ELEMENT while prevented");
	/* The MAP is open, but the operator still cannot take anything out */
	operator("load map E01003L8");
	operator_gets("empty map", "Can't empty map - medium removal prevented");
	host_open_close(1, &r);
	CHECK_GOOD(r, "host CLOSE IMPORT/EXPORT ELEMENT while prevented");
	operator_gets("load map E01004L8", "MAP not opened");
	stop_library();
}

static void move_medium(uint16_t src, uint16_t dest, struct scsi_result *r)
{
	uint8_t c[12] = { 0xa5, 0, 0x00, 0x01 }; /* transport element 1 */

	put_unaligned_be16(src, &c[4]);
	put_unaligned_be16(dest, &c[6]);
	cdb(c, 12, r);
}

/* The host's MOVE MEDIUM out of the MAP and back is not removal by an operator */
static void test_host_move_allowed(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	operator("open map");
	operator("load map E01001L8");
	operator("close map");
	cdb(tur, 6, &r); /* takes the unit attention of the access */
	prevent(1, "PREVENT");
	move_medium(MAP_ADDR, SLOT_ADDR, &r);
	CHECK_GOOD(r, "MOVE MEDIUM from the MAP to a slot while prevented");
	move_medium(SLOT_ADDR, MAP_ADDR, &r);
	CHECK_GOOD(r, "MOVE MEDIUM from the slot to the MAP while prevented");
	stop_library();
}

/* mhvtl-bay-monitor reports a pulled disk with "media gone", which the lock
 * does not refuse, and which the host sees as an import/export access */
static void test_media_gone_while_prevented(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	operator("open map");
	operator("load map E01001L8");
	operator("load map E01002L8");
	operator("close map");
	cdb(tur, 6, &r); /* takes the unit attention of the access */
	prevent(1, "PREVENT");
	operator("media gone E01001L8");
	cdb(tur, 6, &r);
	CHECK_SENSE(r, UNIT_ATTENTION, 0x28, 0x01, "TEST UNIT READY after media gone");
	operator_gets("media gone E01001L8", "barcode not in map");
	/* Only that cartridge went: the other one is still in the MAP */
	prevent(0, "ALLOW");
	operator("open map");
	operator("empty map E01002L8");
	stop_library();
}

/* The disk is pulled while the MAP is open, as the host left it */
static void test_media_gone_map_open(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	host_open_close(0, &r);
	CHECK_GOOD(r, "host OPEN IMPORT/EXPORT ELEMENT");
	operator("load map E01001L8");
	prevent(1, "PREVENT");
	operator("media gone E01001L8");
	cdb(tur, 6, &r);
	CHECK_SENSE(r, UNIT_ATTENTION, 0x28, 0x01, "TEST UNIT READY after media gone");
	operator_gets("media gone E01001L8", "barcode not in map");
	/* The MAP is still open */
	operator("load map E01002L8");
	stop_library();
}

/* Re-reading the configuration (SIGHUP) resets the library and allows removal */
static void test_sighup_allows(void)
{
	struct scsi_result r;

	TEST_ASSERT(start_library() == 0);
	prevent(1, "PREVENT");
	TEST_ASSERT(kill(daemon_ctx.pid, SIGHUP) == 0);
	/* The daemon re-reads from its main loop: wait for the reset */
	for (int i = 0; i < 50; i++) {
		cdb(tur, 6, &r);
		if (r.sam_stat != SAM_STAT_GOOD)
			break;
		usleep(100000);
	}
	CHECK_SENSE(r, UNIT_ATTENTION, 0x29, 0x00, "TEST UNIT READY after SIGHUP");
	operator("open map");
	stop_library();
}

/* Only bit 0 of byte 4 prevents removal */
static void test_prevent_bit_only(void)
{
	TEST_ASSERT(start_library() == 0);
	prevent(0x02, "PREVENT with only bit 1 set");
	operator("open map");
	stop_library();
}

TEST_LIST = {
	{ "open_refused", test_open_refused },
	{ "empty_refused_while_open", test_empty_refused_while_open },
	{ "host_open_close_allowed", test_host_open_close_allowed },
	{ "host_move_allowed", test_host_move_allowed },
	{ "media_gone_while_prevented", test_media_gone_while_prevented },
	{ "media_gone_map_open", test_media_gone_map_open },
	{ "sighup_allows", test_sighup_allows },
	{ "prevent_bit_only", test_prevent_bit_only },
	{ NULL, NULL }
};
