/*
 * Integration tests for SPACE (blocks) in the reverse direction.
 *
 * Starts vtltape (LTO-8) with MHVTL_BACKEND=userland against a generated
 * device.conf and checks sense data and the logical position afterwards.
 *
 * Reference: IBM LTO SCSI Reference GA32-0928-07, SPACE CODE 0h (p.172-173)
 * and Table 17 "Information and ILI Behavior Summary" (Space blocks, p.51).
 */
#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

static int start(void)
{
	const struct drive_type *dt;
	struct scsi_result r;

	for (dt = ALL_DRIVES; dt->name; dt++)
		if (strcmp(dt->name, "LTO-8") == 0)
			break;
	if (!dt->name || gen_test_env(&test_env, dt) < 0)
		return -1;
	if (harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env) != 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	if (harness_load_tape(TEST_MINOR_TAPE, dt->media_barcode) != 0)
		return -1;
	for (int i = 0; i < 100; i++) {
		uint8_t tur[6] = { 0x00 };

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

/* SPACE(6), CODE 0h (blocks); count is two's complement */
static void space_blocks(int32_t count, struct scsi_result *r)
{
	uint8_t cdb[6] = {
		0x11, 0x00,
		(count >> 16) & 0xff, (count >> 8) & 0xff, count & 0xff,
		0x00
	};
	harness_cdb(&daemon_ctx, cdb, 6, r);
}

/* SPACE(6), CODE 1h (filemarks) */
static void space_filemarks(int32_t count, struct scsi_result *r)
{
	uint8_t cdb[6] = {
		0x11, 0x01,
		(count >> 16) & 0xff, (count >> 8) & 0xff, count & 0xff,
		0x00
	};
	harness_cdb(&daemon_ctx, cdb, 6, r);
}

/* READ POSITION short form: returns first block location, sets *bop */
static uint32_t read_position(int *bop)
{
	uint8_t cdb[10] = { 0x34 };
	struct scsi_result r;

	harness_cdb(&daemon_ctx, cdb, 10, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "READ POSITION sam_stat=%d",
		    r.sam_stat);
	*bop = !!(harness_data_buf[0] & 0x80);
	return (uint32_t)harness_data_buf[4] << 24 | harness_data_buf[5] << 16 |
	       harness_data_buf[6] << 8 | harness_data_buf[7];
}

static uint32_t sense_info(const struct scsi_result *r)
{
	return (uint32_t)r->sense[3] << 24 | r->sense[4] << 16 |
	       r->sense[5] << 8 | r->sense[6];
}

static void write_blocks(int n)
{
	uint8_t buf[512];
	struct scsi_result r;

	memset(buf, 0x5a, sizeof(buf));
	for (int i = 0; i < n; i++) {
		harness_tape_write(&daemon_ctx, buf, sizeof(buf), &r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "WRITE sam_stat=%d",
			    r.sam_stat);
	}
}

/*
 * 3 blocks + FM, rewind, SPACE +2, SPACE -10.
 * Table 17 (p.51): 0/0004, EOM set, INFORMATION = count - blocks traversed
 * (10 - 2 = 8), positioned at BOP.
 */
void test_space_back_past_bop(void)
{
	struct scsi_result r;
	uint32_t pos;
	int bop;

	if (!TEST_CHECK(start() == 0)) {
		stop();
		return;
	}

	write_blocks(3);
	harness_tape_write_fm(&daemon_ctx, 1, &r);
	harness_tape_rewind(&daemon_ctx, &r);

	space_blocks(2, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "SPACE +2 sam_stat=%d",
		    r.sam_stat);

	space_blocks(-10, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION,
		    "SPACE -10 sam_stat=%d", r.sam_stat);
	TEST_CHECK_((r.sense[2] & 0x0f) == 0x00 && r.sense[12] == 0x00 &&
		    r.sense[13] == 0x04,
		    "sense %x/%02x/%02x (expected 0/00/04)",
		    r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	TEST_CHECK_((r.sense[2] & 0xc0) == 0x40,
		    "sense[2]=0x%02x (expected EOM, no FM)", r.sense[2]);
	TEST_CHECK_(sense_info(&r) == 8, "INFORMATION=%u (expected 8)",
		    sense_info(&r));

	pos = read_position(&bop);
	TEST_CHECK_(pos == 0 && bop, "position %u bop=%d (expected 0, BOP)",
		    pos, bop);

	stop();
}

/*
 * 2 blocks, FM, 2 blocks, FM (FMs at objects 2 and 5). From object 5
 * SPACE -10 hits the FM at 2 and stops on its BOP side: 0/0001, FM set,
 * INFORMATION = count - blocks spaced over not including the filemark
 * (10 - 2 = 8). SPACE CODE 0h, p.172-173.
 */
void test_space_back_stops_at_filemark(void)
{
	struct scsi_result r;
	uint32_t pos;
	int bop;

	if (!TEST_CHECK(start() == 0)) {
		stop();
		return;
	}

	write_blocks(2);
	harness_tape_write_fm(&daemon_ctx, 1, &r);
	write_blocks(2);
	harness_tape_write_fm(&daemon_ctx, 1, &r);
	harness_tape_rewind(&daemon_ctx, &r);

	space_filemarks(1, &r);
	space_blocks(2, &r);
	pos = read_position(&bop);
	TEST_CHECK_(pos == 5, "start position %u (expected 5)", pos);

	space_blocks(-10, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION,
		    "SPACE -10 sam_stat=%d", r.sam_stat);
	TEST_CHECK_((r.sense[2] & 0x0f) == 0x00 && r.sense[12] == 0x00 &&
		    r.sense[13] == 0x01,
		    "sense %x/%02x/%02x (expected 0/00/01)",
		    r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	TEST_CHECK_((r.sense[2] & 0xc0) == 0x80,
		    "sense[2]=0x%02x (expected FM, no EOM)", r.sense[2]);
	TEST_CHECK_(sense_info(&r) == 8, "INFORMATION=%u (expected 8)",
		    sense_info(&r));

	pos = read_position(&bop);
	TEST_CHECK_(pos == 2 && !bop, "position %u bop=%d (expected 2)",
		    pos, bop);

	/* Same filemark, count within range: residual is 3 - 2 = 1 */
	space_filemarks(1, &r);
	space_blocks(2, &r);
	space_blocks(-3, &r);
	TEST_CHECK_((r.sense[2] & 0x8f) == 0x80 && r.sense[13] == 0x01,
		    "SPACE -3 sense[2]=0x%02x ascq=%02x (expected FM, 0/00/01)",
		    r.sense[2], r.sense[13]);
	TEST_CHECK_(sense_info(&r) == 1, "INFORMATION=%u (expected 1)",
		    sense_info(&r));
	pos = read_position(&bop);
	TEST_CHECK_(pos == 2, "position %u (expected 2)", pos);

	stop();
}

TEST_LIST = {
	{ "space_back_past_bop",          test_space_back_past_bop },
	{ "space_back_stops_at_filemark", test_space_back_stops_at_filemark },
	{ NULL, NULL }
};
