/*
 * test_ssc_fixed_ew.c — Fixed-block WRITE(6) across early warning.
 *
 * A multi-block fixed-mode WRITE(6) that crosses early warning must not
 * drop blocks the host believes were written. The host's accounting
 * (count - INFORMATION when VALID, count on GOOD) has to match the number
 * of blocks that read back.
 *
 * Reference: IBM LTO SCSI Reference GA32-0928-07 (03 Jun 2025),
 * Table 17 "Write in Early Warning" / "Write at EOM", p.51.
 *
 * Usage: ./test_ssc_fixed_ew
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"

#define BLK_SZ		(64 * 1024)
#define BLKS_PER_CMD	8
#define MEDIA_MB	"20"

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

/* LTO-8 with a small cartridge so EW and EOM are reached quickly */
static int start_small_lto(void)
{
	const struct drive_type *dt;
	char cmd[1024];
	char mktape_path[256];

	for (dt = ALL_DRIVES; dt->name; dt++)
		if (!strcmp(dt->name, "LTO-8"))
			break;
	if (!dt->name)
		return -1;
	if (gen_test_env(&test_env, dt) < 0)
		return -1;

	/* Replace the default 500 MB cartridge with a small one */
	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "rm -rf %s/%s && %s%s -C %s -H %s -l %d -m %s -s " MEDIA_MB
		 " -t data -d %s >/dev/null 2>&1",
		 test_env.home_dir, dt->media_barcode,
		 gen_local_lib_prefix(mktape_path), mktape_path,
		 test_env.conf_dir, test_env.home_dir, TEST_MINOR_LIBRARY,
		 dt->media_barcode, dt->density);
	if (system(cmd) != 0)
		return -1;

	if (harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env) != 0)
		return -1;
	harness_clear_ua(&daemon_ctx);

	if (harness_load_tape(TEST_MINOR_TAPE, dt->media_barcode) != 0)
		return -1;
	for (int i = 0; i < 100; i++) {
		uint8_t tur[6] = { 0x00 };
		struct scsi_result r;

		harness_cdb(&daemon_ctx, tur, 6, &r);
		if (r.sam_stat == SAM_STAT_GOOD)
			return 0;
		usleep(100000);
	}
	return -1;
}

static void stop(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

/* MODE SELECT(6): block descriptor only, fixed block length */
static int set_block_length(uint32_t len)
{
	uint8_t cdb[6] = { 0x15, 0x10, 0x00, 0x00, 12, 0x00 };
	uint8_t param[12] = { 0 };
	struct scsi_result r;

	param[2] = 0x10;	/* buffered mode */
	param[3] = 8;		/* block descriptor length */
	put_unaligned_be24(len, &param[9]);
	if (harness_send_cdb_with_data_out(&daemon_ctx, cdb, 6, param,
					   sizeof(param), &r) != 0)
		return -1;
	return r.sam_stat == SAM_STAT_GOOD ? 0 : -1;
}

/* Block n carries its own index, the rest is pseudo-random */
static void fill_block(uint8_t *p, uint32_t n)
{
	uint32_t x = n * 2654435761u + 1;

	for (int i = 0; i < BLK_SZ; i++) {
		x = x * 1103515245u + 12345u;
		p[i] = (uint8_t)(x >> 16);
	}
	put_unaligned_be32(n, p);
}

static uint8_t wbuf[BLKS_PER_CMD * BLK_SZ];
static uint8_t rbuf[BLK_SZ];

void test_fixed_write_across_early_warning(void)
{
	struct scsi_result r;
	uint32_t host_written = 0;
	uint32_t read_back = 0;
	int ew_seen = 0;
	int overflow_seen = 0;

	if (!TEST_CHECK_(start_small_lto() == 0, "start vtltape + load media")) {
		stop();
		return;
	}
	TEST_ASSERT_(set_block_length(BLK_SZ) == 0, "MODE SELECT block length");

	for (int iter = 0; iter < 1000 && !overflow_seen; iter++) {
		uint8_t cdb[6] = { 0x0a, 0x01, 0x00, 0x00, BLKS_PER_CMD, 0x00 };
		uint32_t done;

		for (int b = 0; b < BLKS_PER_CMD; b++)
			fill_block(&wbuf[b * BLK_SZ], host_written + b);

		TEST_ASSERT(harness_send_cdb_with_data_out(&daemon_ctx, cdb, 6,
							   wbuf, sizeof(wbuf), &r) == 0);
		if (r.sam_stat == SAM_STAT_GOOD) {
			host_written += BLKS_PER_CMD;
			continue;
		}
		TEST_ASSERT_(r.sam_stat == SAM_STAT_CHECK_CONDITION,
			     "WRITE sam_stat=%d", r.sam_stat);
		TEST_ASSERT_(r.sense[0] & 0x80, "INFORMATION VALID on WRITE CC");

		done = BLKS_PER_CMD - get_unaligned_be32(&r.sense[3]);
		TEST_ASSERT_(done <= BLKS_PER_CMD, "residual %u within transfer length",
			     get_unaligned_be32(&r.sense[3]));
		host_written += done;

		switch (r.sense[2] & 0x0f) {
		case NO_SENSE:
			TEST_CHECK_(r.sense[2] & SD_EOM, "EW sets EOM");
			ew_seen++;
			break;
		case VOLUME_OVERFLOW:
			overflow_seen = 1;
			break;
		default:
			TEST_CHECK_(0, "unexpected sense %x/%02x%02x",
				    r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
			overflow_seen = 1;
			break;
		}
	}
	TEST_CHECK_(ew_seen > 0, "early warning reported");
	TEST_CHECK_(overflow_seen, "VOLUME OVERFLOW reached");

	TEST_ASSERT(harness_tape_rewind(&daemon_ctx, &r) == 0 &&
		    r.sam_stat == SAM_STAT_GOOD);

	for (;;) {
		uint8_t cdb[6] = { 0x08, 0x01, 0x00, 0x00, 0x01, 0x00 };

		if (harness_send_cdb(&daemon_ctx, cdb, 6, &r, rbuf,
				     sizeof(rbuf)) != 0 ||
		    r.sam_stat != SAM_STAT_GOOD)
			break;
		fill_block(wbuf, read_back);
		if (r.data_len != BLK_SZ || memcmp(rbuf, wbuf, BLK_SZ)) {
			TEST_CHECK_(0, "block %u mismatch (index %u, len %u)",
				    read_back, get_unaligned_be32(rbuf),
				    r.data_len);
			break;
		}
		read_back++;
	}

	TEST_CHECK_(host_written == read_back,
		    "host counted %u blocks written, %u read back",
		    host_written, read_back);
	TEST_MSG("early warning reports: %d", ew_seen);

	stop();
}

TEST_LIST = {
	{ "fixed_write_across_early_warning", test_fixed_write_across_early_warning },
	{ NULL, NULL }
};
