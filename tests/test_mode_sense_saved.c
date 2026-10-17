/*
 * Integration tests for MODE SENSE with PC=11b (saved values).
 *
 * IBM LTO drives report saved mode values (IBM LTO SCSI Reference
 * GA32-0928-07, 4.7.2 and Table 14, p.41-42), as do IBM libraries
 * (TS2900 SCSI Reference GC27-2211-00, Table 36, p.29-30). mhVTL never
 * saves mode pages, so the saved values equal the power-on values.
 * Personalities that do not claim saved values keep failing with
 * ILLEGAL REQUEST / SAVING PARAMETERS NOT SUPPORTED (05/39/00).
 *
 * Uses the userland transport — no kernel modules or root needed.
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

static const struct drive_type *find_drive(const char *name)
{
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++)
		if (!strcmp(dt->name, name))
			return dt;
	return NULL;
}

/* Replace the generated STK L700 library identity with vendor/product */
static int set_library_identity(const char *vendor, const char *product)
{
	char path[512], buf[8192], out[8192 + 128];
	const char *old = " Vendor identification: STK\n"
			  " Product identification: L700\n";
	FILE *fp;
	size_t n;
	char *p;

	snprintf(path, sizeof(path), "%s/device.conf", test_env.conf_dir);
	fp = fopen(path, "r");
	if (!fp)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	buf[n] = '\0';

	p = strstr(buf, old);
	if (!p)
		return -1;
	*p = '\0';
	snprintf(out, sizeof(out),
		 "%s Vendor identification: %s\n Product identification: %s\n%s",
		 buf, vendor, product, p + strlen(old));

	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fputs(out, fp);
	fclose(fp);
	return 0;
}

static int start(const char *drive, const char *type, unsigned minor,
		 const char *lib_vendor, const char *lib_product)
{
	const struct drive_type *dt = find_drive(drive);

	if (!dt)
		return -1;
	gen_cleanup_env(&test_env);
	if (gen_test_env(&test_env, dt) < 0)
		return -1;
	if (lib_vendor && set_library_identity(lib_vendor, lib_product) < 0)
		return -1;
	if (harness_start_env(&daemon_ctx, minor, type, &test_env) < 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	return 0;
}

static void stop(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

/* MODE SENSE(6) or (10), all pages, DBD=0, with page control pc */
static int mode_sense(int ten, int pc, struct scsi_result *r)
{
	uint8_t cdb6[6] = { 0x1a, 0x00, (uint8_t)(pc << 6 | 0x3f), 0x00, 0xff, 0x00 };
	uint8_t cdb10[10] = { 0x5a, 0x00, (uint8_t)(pc << 6 | 0x3f), 0x00,
			      0x00, 0x00, 0x00, 0x04, 0x00, 0x00 };

	memset(r, 0, sizeof(*r));
	return ten ? harness_cdb(&daemon_ctx, cdb10, 10, r) :
		     harness_cdb(&daemon_ctx, cdb6, 6, r);
}

static void check_saved_equals_current(const char *what)
{
	for (int ten = 0; ten <= 1; ten++) {
		struct scsi_result cur, saved;
		static uint8_t cur_data[4096];

		TEST_CHECK(mode_sense(ten, 0, &cur) == 0);
		TEST_CHECK_(cur.sam_stat == SAM_STAT_GOOD,
			    "%s MODE SENSE(%d) PC=0 sam_stat=%d",
			    what, ten ? 10 : 6, cur.sam_stat);
		if (cur.data_len > sizeof(cur_data))
			cur.data_len = sizeof(cur_data);
		memcpy(cur_data, cur.data, cur.data_len);

		TEST_CHECK(mode_sense(ten, 3, &saved) == 0);
		TEST_CHECK_(saved.sam_stat == SAM_STAT_GOOD,
			    "%s MODE SENSE(%d) PC=3 sam_stat=%d sense %02x/%02x/%02x",
			    what, ten ? 10 : 6, saved.sam_stat,
			    saved.sense[2] & 0x0f, saved.sense[12], saved.sense[13]);
		TEST_CHECK_(saved.data_len == cur.data_len &&
			    !memcmp(saved.data, cur_data, cur.data_len),
			    "%s MODE SENSE(%d) PC=3 data (%u bytes) == PC=0 data (%u bytes)",
			    what, ten ? 10 : 6, saved.data_len, cur.data_len);
	}
}

static void check_saving_unsupported(const char *what)
{
	for (int ten = 0; ten <= 1; ten++) {
		struct scsi_result r;

		TEST_CHECK(mode_sense(ten, 3, &r) == 0);
		TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION &&
			    (r.sense[2] & 0x0f) == 0x05 &&
			    r.sense[12] == 0x39 && r.sense[13] == 0x00,
			    "%s MODE SENSE(%d) PC=3 sam_stat=%d sense %02x/%02x/%02x (expected 05/39/00)",
			    what, ten ? 10 : 6, r.sam_stat,
			    r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	}
}

void test_ibm_lto_saved_values(void)
{
	const char *drives[] = { "LTO-5", "LTO-8", NULL };

	for (int i = 0; drives[i]; i++) {
		TEST_CASE(drives[i]);
		if (start(drives[i], "tape", TEST_MINOR_TAPE, NULL, NULL) < 0) {
			TEST_CHECK_(0, "daemon start for %s", drives[i]);
			stop();
			continue;
		}
		check_saved_equals_current(drives[i]);
		stop();
	}
}

void test_other_drive_saving_unsupported(void)
{
	const char *drives[] = { "HP-LTO6", "T10000C", NULL };

	for (int i = 0; drives[i]; i++) {
		TEST_CASE(drives[i]);
		if (start(drives[i], "tape", TEST_MINOR_TAPE, NULL, NULL) < 0) {
			TEST_CHECK_(0, "daemon start for %s", drives[i]);
			stop();
			continue;
		}
		check_saving_unsupported(drives[i]);
		stop();
	}
}

void test_ibm_library_saved_values(void)
{
	if (start("LTO-8", "library", TEST_MINOR_LIBRARY, "IBM", "03584L32") < 0) {
		TEST_CHECK_(0, "library start");
		stop();
		return;
	}
	check_saved_equals_current("IBM 03584L32");
	stop();
}

void test_stk_library_saving_unsupported(void)
{
	if (start("LTO-8", "library", TEST_MINOR_LIBRARY, NULL, NULL) < 0) {
		TEST_CHECK_(0, "library start");
		stop();
		return;
	}
	check_saving_unsupported("STK L700");
	stop();
}

TEST_LIST = {
	{ "ibm_lto_saved_values",           test_ibm_lto_saved_values },
	{ "other_drive_saving_unsupported", test_other_drive_saving_unsupported },
	{ "ibm_library_saved_values",       test_ibm_library_saved_values },
	{ "stk_library_saving_unsupported", test_stk_library_saving_unsupported },
	{ NULL, NULL }
};
