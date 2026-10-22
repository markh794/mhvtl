/*
 * test_log_volstat_strings.c — string parameters of LOG SENSE page 17h.
 *
 * Volume statistics parameters 0040h (volume serial number), 0042h (volume
 * barcode), 0043h (volume manufacturer) and 0045h (volume personality) come
 * from the loaded cartridge: IBM LTO SCSI Reference GA32-0928-07, Table 279,
 * pp. 301-303. The personality is the layout that table gives per LTO
 * generation ("Ultrium-5", "LTOL8G8", "LTOM8G8", "LTOLA62", ...).
 *
 * Each case starts vtltape with one cartridge, loads it, issues LOG SENSE
 * 17h and walks the returned parameters by their length fields. A non-LTO
 * format and an unloaded drive must report the strings as zero.
 *
 * Usage: ./test_log_volstat_strings
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"
#include "vtllib.h"
#include "vtlcart.h"

/* libvtlscsi expects each executable to supply this symbol. */
char mhvtl_driver_name[] = "test_log_volstat_strings";

#define VOLSTAT_BUFSZ 4096

struct volstat_case {
	const char *vendor;
	const char *product;
	const char *density;	 /* mktape -d */
	const char *media_type;	 /* mktape -t */
	const char *barcode;
	int			patch_density; /* MediumDensityCode to force, or -1 */
	const char *personality; /* expected 0045h, NULL for all zero */
};

/* Find parameter 'code' in a LOG SENSE page. Returns its length, or -1. */
static int find_param(const uint8_t *pg, uint32_t len, uint16_t code,
					  const uint8_t **val)
{
	uint32_t end = 4 + get_unaligned_be16(&pg[2]);
	uint32_t off = 4;

	if (end > len)
		end = len;
	while (off + 4 <= end) {
		uint16_t c = get_unaligned_be16(&pg[off]);
		uint8_t	 l = pg[off + 3];

		if (off + 4 + l > end)
			return -1;
		if (c == code) {
			*val = &pg[off + 4];
			return l;
		}
		off += 4 + l;
	}
	return -1;
}

static int log_sense_17(struct harness_daemon *d, uint8_t *buf, uint32_t *len)
{
	uint8_t cdb[10] = { 0x4d, 0, 0x40 | 0x17, 0, 0, 0, 0,
						VOLSTAT_BUFSZ >> 8, VOLSTAT_BUFSZ & 0xff, 0 };
	struct scsi_result r;

	memset(buf, 0xee, VOLSTAT_BUFSZ);
	if (harness_send_cdb(d, cdb, 10, &r, buf, VOLSTAT_BUFSZ) < 0)
		return -1;
	if (r.sam_stat != SAM_STAT_GOOD || (buf[0] & 0x3f) != 0x17)
		return -1;
	*len = r.data_len < VOLSTAT_BUFSZ ? r.data_len : VOLSTAT_BUFSZ;
	return 0;
}

/* Check parameter 'code' is 'size' bytes holding 'want' padded with spaces,
 * or all zero when want is NULL. */
static void check_string(const uint8_t *pg, uint32_t len, uint16_t code,
						 int size, const char *want)
{
	const uint8_t *v = NULL;
	char		   exp[64];
	int			   l = find_param(pg, len, code, &v);

	TEST_ASSERT_(l == size, "%04Xh present, length %d (want %d)", code, l, size);
	if (want)
		snprintf(exp, sizeof(exp), "%-*s", size, want);
	else
		memset(exp, 0, sizeof(exp));
	TEST_CHECK_(memcmp(v, exp, size) == 0, "%04Xh = '%.*s', want '%s'",
				code, size, (const char *)v, want ? want : "(zero)");
}

/* Rewrite the cartridge's MAM density, as edit_tape does for its fields */
static int patch_density(struct test_env *env, const char *barcode, uint8_t density)
{
	uint8_t sam_stat = SAM_STAT_GOOD;
	int		out		 = dup(STDOUT_FILENO);
	int		null	 = open("/dev/null", O_WRONLY);
	int		rc;

	setenv("MHVTL_CONFIG_PATH", env->conf_dir, 1);
	setenv("MHVTL_HOME_PATH", env->home_dir, 1);
	find_media_home_directory(env->conf_dir, TEST_MINOR_LIBRARY);

	/* load_tape() prints "mam from <path>" to stdout */
	fflush(stdout);
	if (out >= 0 && null >= 0)
		dup2(null, STDOUT_FILENO);
	rc = load_tape(barcode, &sam_stat);
	fflush(stdout);
	if (out >= 0 && null >= 0)
		dup2(out, STDOUT_FILENO);
	if (out >= 0)
		close(out);
	if (null >= 0)
		close(null);
	if (rc)
		return -1;
	mam.MediumDensityCode	 = density;
	mam.FormattedDensityCode = density;
	if (rewriteMAM(&sam_stat))
		return -1;
	unload_tape(&sam_stat);
	return 0;
}

static int remake_tape(struct test_env *env, const struct volstat_case *c)
{
	char cmd[1024];
	char mktape_path[256];

	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;
	snprintf(cmd, sizeof(cmd), "rm -rf %s/%s", env->home_dir, c->barcode);
	if (system(cmd) != 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
			 "%s%s -C %s -H %s -l %d -m %s -s 500 -t %s -d %s >/dev/null 2>&1",
			 gen_local_lib_prefix(mktape_path), mktape_path,
			 env->conf_dir, env->home_dir, TEST_MINOR_LIBRARY,
			 c->barcode, c->media_type, c->density);
	return system(cmd) == 0 ? 0 : -1;
}

static int wait_ready(struct harness_daemon *d)
{
	struct scsi_result r;
	uint8_t			   tur[6] = { 0x00 };

	for (int i = 0; i < 100; i++) {
		harness_cdb(d, tur, 6, &r);
		if (r.sam_stat == SAM_STAT_GOOD)
			return 1;
		usleep(100000);
	}
	return 0;
}

static void run_case(const struct volstat_case *c, int unload_after)
{
	struct drive_type dt = { "", c->vendor, c->product, "0100",
							 c->barcode, c->density, 1, 1, 1 };
	struct test_env		  env;
	struct harness_daemon d;
	static uint8_t		  buf[VOLSTAT_BUFSZ];
	uint32_t			  len = 0;
	const uint8_t		 *v	  = NULL;
	char				  serial[40];

	memset(&env, 0, sizeof(env));
	TEST_ASSERT_(gen_test_env(&env, &dt) == 0, "gen_test_env");
	TEST_ASSERT_(remake_tape(&env, c) == 0, "mktape -t %s -d %s",
				 c->media_type, c->density);
	if (c->patch_density >= 0)
		TEST_ASSERT_(patch_density(&env, c->barcode, c->patch_density) == 0,
					 "set density %02xh", c->patch_density);

	if (harness_start_env(&d, TEST_MINOR_TAPE, "tape", &env) < 0) {
		TEST_CHECK_(0, "vtltape did not start");
		goto out;
	}
	harness_clear_ua(&d);
	TEST_CHECK_(harness_load_tape(TEST_MINOR_TAPE, c->barcode) == 0 &&
					wait_ready(&d),
				"%s %s loaded and ready", c->product, c->barcode);

	TEST_ASSERT_(log_sense_17(&d, buf, &len) == 0, "LOG SENSE 17h");

	/* mktape writes "<barcode>_<time>" as the medium serial number */
	TEST_ASSERT(find_param(buf, len, 0x0040, &v) == 32);
	snprintf(serial, sizeof(serial), "%s_", c->barcode);
	TEST_CHECK_(memcmp(v, serial, strlen(serial)) == 0 && v[31] == ' ',
				"0040h = '%.32s', want '%s<time>' space padded",
				(const char *)v, serial);
	check_string(buf, len, 0x0041, 8, NULL);
	check_string(buf, len, 0x0042, 32, c->barcode);
	check_string(buf, len, 0x0043, 8, "linuxVTL");
	check_string(buf, len, 0x0044, 4, NULL);
	check_string(buf, len, 0x0045, 9, c->personality);

	if (unload_after) {
		TEST_CHECK_(harness_unload_tape(TEST_MINOR_TAPE) == 0, "unload");
		usleep(200000);
		harness_clear_ua(&d);
		TEST_ASSERT_(log_sense_17(&d, buf, &len) == 0, "LOG SENSE 17h, unloaded");
		check_string(buf, len, 0x0040, 32, NULL);
		check_string(buf, len, 0x0042, 32, NULL);
		check_string(buf, len, 0x0043, 8, NULL);
		check_string(buf, len, 0x0045, 9, NULL);
	}

out:
	harness_stop(&d);
	gen_cleanup_env(&env);
}

#define IBM "IBM"

void test_lto5(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TD5", "LTO5", "data",
										   "E01001L5", -1, "Ultrium-5" };
	run_case(&c, 0);
}

void test_lto7(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TD7", "LTO7", "data",
										   "E01001L7", -1, "Ultrium-7" };
	run_case(&c, 0);
}

void test_lto8(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TD8", "LTO8", "data",
										   "E01001L8", -1, "LTOL8G8" };
	run_case(&c, 0);
}

void test_lto8_worm(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TD8", "LTO8", "WORM",
										   "E01001LY", -1, "LTOLYG8" };
	run_case(&c, 0);
}

/* An LTO-7 cartridge formatted by an LTO-8 drive at density 5Dh */
void test_lto8_m8(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TD8", "LTO7", "data",
										   "E01001M8", medium_density_code_lto8_m8,
										   "LTOM8G8" };
	run_case(&c, 0);
}

void test_lto9(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TD9", "LTO9", "data",
										   "E01001L9", -1, "LTOL9G9" };
	run_case(&c, 0);
}

void test_lto9_worm(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TD9", "LTO9", "WORM",
										   "E01001LZ", -1, "LTOLZG9" };
	run_case(&c, 0);
}

void test_lto10(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TDA", "LTO10", "data",
										   "E01001LA", -1, "LTOLA62" };
	run_case(&c, 0);
}

void test_lto10_worm(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TDA", "LTO10", "WORM",
										   "E01001LH", -1, "LTOLH62" };
	run_case(&c, 0);
}

/* GA32-0928-07 has no cartridge type for the 40TB Premium cartridge */
void test_lto10p_empty(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TDA", "LTO10P", "data",
										   "E01001LA", -1, NULL };
	run_case(&c, 0);
}

/* No personality is documented before LTO-5 */
void test_lto4_empty(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TD4", "LTO4", "data",
										   "E01001L4", -1, NULL };
	run_case(&c, 0);
}

/* HPE documents 0045h without values; HP drives report IBM's */
void test_hp_lto6(void)
{
	static const struct volstat_case c = { "HP", "Ultrium 6-SCSI", "LTO6", "data",
										   "E01001L6", -1, "Ultrium-6" };
	run_case(&c, 0);
}

void test_t10000_empty(void)
{
	static const struct volstat_case c = { "STK", "T10000C", "T10KC", "data",
										   "G03031TC", -1, NULL };
	run_case(&c, 0);
}

/* After unload the strings must not carry the last cartridge's values */
void test_unloaded_empty(void)
{
	static const struct volstat_case c = { IBM, "ULT3580-TD8", "LTO8", "data",
										   "E01001L8", -1, "LTOL8G8" };
	run_case(&c, 1);
}

TEST_LIST = {
	{ "lto5", test_lto5 },
	{ "lto7", test_lto7 },
	{ "lto8", test_lto8 },
	{ "lto8_worm", test_lto8_worm },
	{ "lto8_m8", test_lto8_m8 },
	{ "lto9", test_lto9 },
	{ "lto9_worm", test_lto9_worm },
	{ "lto10", test_lto10 },
	{ "lto10_worm", test_lto10_worm },
	{ "lto10p_empty", test_lto10p_empty },
	{ "lto4_empty", test_lto4_empty },
	{ "hp_lto6", test_hp_lto6 },
	{ "t10000_empty", test_t10000_empty },
	{ "unloaded_empty", test_unloaded_empty },
	{ NULL, NULL }
};
