/*
 * test_ssc_eom.c — variable-block WRITE refused at physical end of medium.
 *
 * Writes to a small LTO-8 cartridge until the drive answers
 * CHECK CONDITION VOLUME OVERFLOW / EOM (0D/00/02). Nothing is written by
 * that command, so the sense INFORMATION field must hold the requested
 * transfer length with VALID set: IBM LTO SCSI Reference GA32-0928-07
 * (8th ed.), Table 17 "Write at EOM", p.51. Linux st derives the residual
 * from INFORMATION, so INFORMATION 0 would claim the block was written.
 *
 * The cartridge is then read back: the number of blocks on tape must equal
 * the number of WRITEs accepted with GOOD or NO SENSE/EOM (0/00/02,
 * early warning).
 *
 * Usage: ./test_ssc_eom
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"

#define EOM_BLOCK_SZ	(64 * 1024)
#define EOM_TAPE_MB	4
#define EOM_MAX_WRITES	1000

static uint32_t xorshift(uint32_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 17;
	*s ^= *s << 5;
	return *s;
}

/* Replace the cartridge gen_test_env() made with a small one */
static int make_small_tape(struct test_env *env, const struct drive_type *dt)
{
	char cmd[1024];
	char mktape_path[256];

	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;

	snprintf(cmd, sizeof(cmd), "rm -rf %s/%s", env->home_dir,
		 dt->media_barcode);
	if (system(cmd) != 0)
		return -1;

	snprintf(cmd, sizeof(cmd),
		 "%s%s -C %s -H %s -l %d -m %s -s %d -t data -d %s >/dev/null 2>&1",
		 gen_local_lib_prefix(mktape_path), mktape_path,
		 env->conf_dir, env->home_dir, TEST_MINOR_LIBRARY,
		 dt->media_barcode, EOM_TAPE_MB, dt->density);
	return system(cmd) == 0 ? 0 : -1;
}

void test_variable_write_at_eom(void)
{
	const struct drive_type *dt = NULL;
	struct test_env env;
	struct harness_daemon d;
	struct scsi_result r;
	static uint8_t buf[EOM_BLOCK_SZ];
	uint32_t seed = 0x12345678;
	int accepted = 0, read_back = 0, hit_eom = 0, ready = 0;
	int i, k;

	for (const struct drive_type *t = ALL_DRIVES; t->name; t++)
		if (strcmp(t->name, "LTO-8") == 0)
			dt = t;
	TEST_ASSERT(dt != NULL);

	memset(&env, 0, sizeof(env));
	TEST_ASSERT_(gen_test_env(&env, dt) == 0, "gen_test_env");
	TEST_ASSERT_(make_small_tape(&env, dt) == 0, "mktape -s %d", EOM_TAPE_MB);

	if (harness_start_env(&d, TEST_MINOR_TAPE, "tape", &env) < 0) {
		TEST_CHECK_(0, "vtltape did not start");
		harness_stop(&d);
		gen_cleanup_env(&env);
		return;
	}
	harness_clear_ua(&d);

	if (harness_load_tape(TEST_MINOR_TAPE, dt->media_barcode) == 0) {
		for (i = 0; i < 100; i++) {
			uint8_t tur[6] = { 0x00 };

			harness_cdb(&d, tur, 6, &r);
			if (r.sam_stat == SAM_STAT_GOOD) {
				ready = 1;
				break;
			}
			usleep(100000);
		}
	}
	TEST_CHECK_(ready, "tape loaded and ready");
	if (!ready)
		goto out;

	for (i = 0; i < EOM_MAX_WRITES; i++) {
		/* Incompressible payload, so the tape fills at a known rate */
		for (k = 0; k < EOM_BLOCK_SZ; k += 4) {
			uint32_t v = xorshift(&seed);

			memcpy(&buf[k], &v, 4);
		}

		TEST_ASSERT(harness_tape_write(&d, buf, EOM_BLOCK_SZ, &r) == 0);
		if (r.sam_stat == SAM_STAT_GOOD) {
			accepted++;
			continue;
		}
		TEST_ASSERT_(r.sam_stat == SAM_STAT_CHECK_CONDITION,
			     "write %d: sam_stat=%d", i, r.sam_stat);

		/* Early warning: block written, NO SENSE + EOM */
		if ((r.sense[2] & 0x0f) == NO_SENSE && r.sense[12] == 0 &&
		    r.sense[13] == 0x02) {
			accepted++;
			continue;
		}

		hit_eom = 1;
		TEST_CHECK_((r.sense[2] & 0x0f) == VOLUME_OVERFLOW &&
			    (r.sense[2] & SD_EOM) &&
			    r.sense[12] == 0x00 && r.sense[13] == 0x02,
			    "write %d: sense %02x/%02x/%02x, expected 0D/00/02 + EOM",
			    i, r.sense[2], r.sense[12], r.sense[13]);
		TEST_CHECK_(r.sense[0] & SD_VALID,
			    "VALID bit set (sense[0]=0x%02x)", r.sense[0]);
		TEST_CHECK_(get_unaligned_be32(&r.sense[3]) == EOM_BLOCK_SZ,
			    "INFORMATION=%u, expected transfer length %u",
			    get_unaligned_be32(&r.sense[3]), EOM_BLOCK_SZ);
		break;
	}
	TEST_CHECK_(hit_eom, "reached EOM within %d writes", EOM_MAX_WRITES);
	TEST_MSG("blocks accepted: %d", accepted);

	harness_tape_rewind(&d, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "REWIND");

	for (i = 0; i <= EOM_MAX_WRITES; i++) {
		harness_tape_read(&d, buf, EOM_BLOCK_SZ, &r);
		if (r.sam_stat != SAM_STAT_GOOD)
			break;
		read_back++;
	}
	TEST_CHECK_(read_back == accepted,
		    "blocks read back %d == blocks accepted %d",
		    read_back, accepted);

out:
	harness_stop(&d);
	gen_cleanup_env(&env);
}

TEST_LIST = {
	{ "variable_write_at_eom", test_variable_write_at_eom },
	{ NULL, NULL }
};
