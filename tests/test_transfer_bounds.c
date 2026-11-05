/*
 * test_transfer_bounds.c — MODE SELECT and fixed-mode transfers stay within
 * the 2 MiB command buffer (#347, #348).
 *
 * MODE SELECT (GA32-0928-07 §5.6.1, pp.359-362, and p.97):
 *   - BLOCK DESCRIPTOR LENGTH other than 0 or 8: ILLEGAL REQUEST, INVALID
 *     FIELD IN PARAMETER LIST, field pointer on BLOCK DESCRIPTOR LENGTH;
 *   - a parameter list that truncates the header or the block descriptor:
 *     ILLEGAL REQUEST, PARAMETER LIST LENGTH ERROR;
 *   - BLOCK LENGTH above the READ BLOCK LIMITS maximum: INVALID FIELD IN
 *     PARAMETER LIST, field pointer on BLOCK LENGTH;
 *   - a parameter list length of zero is not an error;
 *   - an invalid list changes nothing.
 *
 * Fixed-mode WRITE(6), READ(6) and VERIFY(6) whose block length (plus the 4
 * bytes of protection information a read appends with LBP_R) times count
 * exceeds the buffer, and a variable WRITE(6) larger than it: ILLEGAL REQUEST, INVALID FIELD IN CDB on TRANSFER
 * LENGTH, nothing read or written. A variable-mode READ larger than any
 * block is still accepted (§4.11, p.48).
 *
 * Every refused command must leave the daemon running.
 *
 * Usage: ./test_transfer_bounds
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"

#define MIB			(1024 * 1024)
#define BUF_SZ		(2 * MIB) /* vtltape's command buffer, READ BLOCK LIMITS max */
#define KEY_ILLEGAL 0x05
#define KEY_BLANK	0x08

static struct harness_daemon d;
static struct test_env		 env;
static uint8_t				*data;

static int setup(void) {
	const struct drive_type *dt;

	if (!data && !(data = calloc(1, BUF_SZ)))
		return -1;

	for (dt = ALL_DRIVES; dt->name; dt++)
		if (!strcmp(dt->name, "LTO-8"))
			break;
	if (!dt->name || gen_test_env(&env, dt) < 0)
		return -1;
	if (harness_start_env(&d, TEST_MINOR_TAPE, "tape", &env) < 0)
		return -1;
	harness_clear_ua(&d);
	if (harness_load_tape(TEST_MINOR_TAPE, dt->media_barcode) < 0)
		return -1;
	for (int i = 0; i < 100; i++) {
		uint8_t			   tur[6] = {0x00};
		struct scsi_result r;

		harness_cdb(&d, tur, 6, &r);
		if (r.sam_stat == SAM_STAT_GOOD)
			return 0;
		usleep(100000);
	}
	return -1;
}

static void teardown(void) {
	harness_stop(&d);
	gen_cleanup_env(&env);
}

static int alive(void) {
	uint8_t			   tur[6] = {0x00};
	struct scsi_result r;

	return harness_cdb(&d, tur, 6, &r) == 0 && r.sam_stat == SAM_STAT_GOOD;
}

static int sense_is(const struct scsi_result *r, uint8_t key, uint8_t asc) {
	return r->sam_stat == SAM_STAT_CHECK_CONDITION && (r->sense[2] & 0x0f) == key &&
		   r->sense[12] == asc && r->sense[13] == 0x00;
}

/* SKSV set: CD bit and field pointer; otherwise -1 */
static int field_pointer(const struct scsi_result *r, int *cd) {
	if (!(r->sense[15] & 0x80))
		return -1;
	*cd = (r->sense[15] & 0x40) ? 1 : 0;
	return (r->sense[16] << 8) | r->sense[17];
}

#define CHECK_REFUSED(r, key, asc, want_cd, want_fp)                                    \
	do {                                                                                \
		int cd_ = -1, fp_ = field_pointer(&(r), &cd_);                                  \
		TEST_CHECK_(sense_is(&(r), key, asc), "sam_stat %u sense %x/%02x/%02x",         \
					(r).sam_stat, (r).sense[2] & 0x0f, (r).sense[12], (r).sense[13]);   \
		if ((want_fp) >= 0)                                                             \
			TEST_CHECK_(fp_ == (want_fp) && cd_ == (want_cd), "fp %d cd %d", fp_, cd_); \
		TEST_CHECK_(alive(), "vtltape still answers");                                  \
	} while (0)

static void mode_select6(const uint8_t *param, uint8_t len, struct scsi_result *r) {
	uint8_t cdb[6] = {0x15, 0x10, 0x00, 0x00, len, 0x00};

	harness_send_cdb_with_data_out(&d, cdb, 6, param, len, r);
}

static void mode_select10(const uint8_t *param, uint16_t len, struct scsi_result *r) {
	uint8_t cdb[10] = {0x55, 0x10, 0, 0, 0, 0, 0, len >> 8, len & 0xff, 0};

	harness_send_cdb_with_data_out(&d, cdb, 10, param, len, r);
}

/* MODE SENSE(6) block descriptor BLOCK LENGTH */
static uint32_t block_length(void) {
	uint8_t			   cdb[6] = {0x1a, 0x00, 0x10, 0x00, 0xff, 0x00};
	uint8_t			   ms[256];
	struct scsi_result r;

	harness_send_cdb(&d, cdb, 6, &r, ms, sizeof(ms));
	if (r.sam_stat != SAM_STAT_GOOD || ms[3] != 8)
		return 0xffffffff;
	return get_unaligned_be24(&ms[9]);
}

/* MODE SELECT(6) header + block descriptor with BLOCK LENGTH 'blk' */
static void set_block_length(uint32_t blk, struct scsi_result *r) {
	uint8_t param[12] = {0};

	param[3] = 8;
	param[4] = 0x7f; /* do not change density */
	put_unaligned_be24(blk, &param[9]);
	mode_select6(param, sizeof(param), r);
}

static void cdb6(uint8_t op, uint8_t b1, uint32_t len, uint8_t *buf, size_t buf_sz,
				 struct scsi_result *r) {
	uint8_t cdb[6] = {op, b1, (len >> 16) & 0xff, (len >> 8) & 0xff, len & 0xff, 0};

	if (op == 0x0a)
		harness_send_cdb_with_data_out(&d, cdb, 6, buf, buf_sz, r);
	else
		harness_send_cdb(&d, cdb, 6, r, buf, buf_sz);
}

static void rewind_tape(void) {
	uint8_t			   cdb[6] = {0x01, 0, 0, 0, 0, 0};
	struct scsi_result r;

	harness_cdb(&d, cdb, 6, &r);
}

/* Nothing was written: the first read at BOP finds EOD */
static void check_tape_empty(void) {
	struct scsi_result r;

	rewind_tape();
	cdb6(0x08, 0x00, 512, data, 512, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION && (r.sense[2] & 0x0f) == KEY_BLANK,
				"tape is empty (sense %x/%02x/%02x)", r.sense[2] & 0x0f, r.sense[12],
				r.sense[13]);
}

void test_mode_select_block_descriptor(void) {
	uint8_t			   param[300];
	struct scsi_result r;
	uint32_t		   before;

	TEST_ASSERT(setup() == 0);
	before = block_length();

	TEST_CASE("MODE SELECT(6), BLOCK DESCRIPTOR LENGTH 16");
	memset(param, 0x41, sizeof(param));
	memset(param, 0, 4);
	param[3] = 16;
	mode_select6(param, 20, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x26, 0, 3);

	TEST_CASE("MODE SELECT(6), BLOCK DESCRIPTOR LENGTH 255");
	param[3] = 255;
	mode_select6(param, 255, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x26, 0, 3);

	TEST_CASE("MODE SELECT(10), BLOCK DESCRIPTOR LENGTH 256");
	memset(param, 0x41, sizeof(param));
	memset(param, 0, 8);
	put_unaligned_be16(256, &param[6]);
	mode_select10(param, 264, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x26, 0, 6);

	TEST_CASE("header truncated");
	memset(param, 0, sizeof(param));
	mode_select6(param, 2, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x1a, 0, -1);

	TEST_CASE("block descriptor truncated");
	param[3] = 8;
	mode_select6(param, 8, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x1a, 0, -1);

	TEST_CASE("BLOCK LENGTH above the buffer");
	set_block_length(BUF_SZ + 1, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x26, 0, 9);

	TEST_CASE("nothing changed");
	TEST_CHECK_(block_length() == before, "block length %u, was %u", block_length(), before);

	TEST_CASE("MODE SELECT(10) header truncated");
	memset(param, 0, sizeof(param));
	mode_select10(param, 6, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x1a, 0, -1);

	TEST_CASE("MODE SELECT(10) block descriptor truncated");
	param[7] = 8;
	mode_select10(param, 12, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x1a, 0, -1);

	TEST_CASE("MODE SELECT(10) BLOCK LENGTH above the buffer");
	param[8] = 0x7f;
	put_unaligned_be24(BUF_SZ + 1, &param[13]);
	mode_select10(param, 16, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x26, 0, 13);

	TEST_CASE("SP set, parameter list length 0");
	{
		uint8_t cdb[6] = {0x15, 0x11, 0, 0, 0, 0};

		harness_send_cdb_with_data_out(&d, cdb, 6, param, 0, &r);
		CHECK_REFUSED(r, KEY_ILLEGAL, 0x24, 1, 1);
	}

	TEST_CASE("nothing changed");
	TEST_CHECK_(block_length() == before, "block length %u, was %u", block_length(), before);

	/* A variable WRITE leaves a mode parameter list with BLOCK LENGTH 1024 in
	 * the command buffer; an empty MODE SELECT must not apply it. */
	TEST_CASE("parameter list length 0");
	memset(param, 0, sizeof(param));
	param[3] = 8;
	param[4] = 0x7f;
	put_unaligned_be24(1024, &param[9]);
	cdb6(0x0a, 0x00, 12, param, 12, &r);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	mode_select6(param, 0, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "sam_stat %u", r.sam_stat);
	TEST_CHECK_(block_length() == before, "block length %u, was %u", block_length(), before);
	TEST_CHECK(alive());

	TEST_CASE("BLOCK LENGTH equal to the buffer");
	set_block_length(BUF_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "sam_stat %u sense %02x/%02x", r.sam_stat,
				r.sense[12], r.sense[13]);
	TEST_CHECK_(block_length() == BUF_SZ, "block length %u", block_length());
	teardown();
}

void test_fixed_write_bounds(void) {
	struct scsi_result r;

	TEST_ASSERT(setup() == 0);

	TEST_CASE("3 x 1 MiB");
	set_block_length(MIB, &r);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	cdb6(0x0a, 0x01, 3, data, BUF_SZ, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x24, 1, 2);

	TEST_CASE("2048 x 2 MiB, 4 GiB wraps to 0 in 32 bits");
	set_block_length(BUF_SZ, &r);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	cdb6(0x0a, 0x01, 2048, data, BUF_SZ, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x24, 1, 2);

	TEST_CASE("variable WRITE(6) of 2 MiB + 1");
	cdb6(0x0a, 0x00, BUF_SZ + 1, data, BUF_SZ, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x24, 1, 2);

	TEST_CASE("nothing written");
	check_tape_empty();

	TEST_CASE("1 x 2 MiB still works");
	cdb6(0x0a, 0x01, 1, data, BUF_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "sam_stat %u sense %x/%02x/%02x", r.sam_stat,
				r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	teardown();
}

void test_fixed_read_bounds(void) {
	struct scsi_result r;

	TEST_ASSERT(setup() == 0);
	for (int i = 0; i < BUF_SZ; i++)
		data[i] = (uint8_t)(i * 31);

	/* three 1 MiB blocks on tape */
	set_block_length(MIB, &r);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	cdb6(0x0a, 0x01, 2, data, BUF_SZ, &r);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	cdb6(0x0a, 0x01, 1, data, MIB, &r);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	rewind_tape();

	TEST_CASE("READ(6) 3 x 1 MiB");
	cdb6(0x08, 0x01, 3, data, BUF_SZ, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x24, 1, 2);

	TEST_CASE("VERIFY(6) 3 x 1 MiB");
	cdb6(0x13, 0x01, 3, NULL, 0, &r);
	CHECK_REFUSED(r, KEY_ILLEGAL, 0x24, 1, 2);

	TEST_CASE("READ(6) 2 x 1 MiB still works");
	rewind_tape();
	cdb6(0x08, 0x01, 2, data, BUF_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && r.data_len == BUF_SZ,
				"sam_stat %u data_len %u sense %02x %x/%02x/%02x info %u", r.sam_stat, r.data_len, r.sense[2], r.sense[2] & 0x0f, r.sense[12], r.sense[13], get_unaligned_be32(&r.sense[3]));

	/* §4.11: a variable read larger than the block is accepted, ILI with
	 * INFORMATION = transfer length - block length */
	TEST_CASE("variable READ(6) of 16 MiB - 1 over a 1 MiB block");
	cdb6(0x08, 0x00, 0xffffff, data, BUF_SZ, &r);
	TEST_CHECK_(r.data_len == MIB, "data_len %u", r.data_len);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION && (r.sense[2] & 0x20) &&
					get_unaligned_be32(&r.sense[3]) == 0xffffff - MIB,
				"sam_stat %u ILI %d INFORMATION %u", r.sam_stat, !!(r.sense[2] & 0x20),
				get_unaligned_be32(&r.sense[3]));
	TEST_CHECK(alive());
	teardown();
}

/* MODE SELECT the Control Extension subpage F0h: LBP method CRC32C, LBP_R */
static void set_lbp_read(struct scsi_result *r) {
	uint8_t	 param[4 + 32] = {0};
	uint8_t *pg			   = &param[4];

	pg[0] = 0x4a; /* SPF, page 0Ah */
	pg[1] = 0xf0;
	put_unaligned_be16(28, &pg[2]);
	pg[4] = 0x02; /* CRC32C */
	pg[5] = 0x04; /* protection information length */
	pg[6] = 0x40; /* LBP_R */
	mode_select6(param, sizeof(param), r);
}

/* With LBP_R the block length includes the 4 bytes of protection
 * information. 512-byte blocks read with BLOCK LENGTH 516 fill it exactly;
 * read with BLOCK LENGTH 512 they are overlength, and their protection
 * information must not spill past each block into the next or past the
 * buffer. */
void test_fixed_read_lbp_bounds(void) {
	struct scsi_result r;

	TEST_ASSERT(setup() == 0);
	for (int i = 0; i < BUF_SZ; i++)
		data[i] = (uint8_t)(i * 31 + i / 512);
	set_block_length(512, &r);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	cdb6(0x0a, 0x01, BUF_SZ / 512, data, BUF_SZ, &r);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	set_lbp_read(&r);
	TEST_ASSERT_(r.sam_stat == SAM_STAT_GOOD, "LBP_R set (sense %x/%02x/%02x)",
				 r.sense[2] & 0x0f, r.sense[12], r.sense[13]);

	TEST_CASE("READ(6) 4096 x 512, overlength with protection information");
	rewind_tape();
	cdb6(0x08, 0x01, BUF_SZ / 512, data, BUF_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION && (r.sense[2] & 0x20),
				"sam_stat %u ILI %d", r.sam_stat, !!(r.sense[2] & 0x20));
	TEST_CHECK_(r.data_len <= BUF_SZ, "data_len %u", r.data_len);
	TEST_CHECK(alive());

	TEST_CASE("READ(6) 4064 x 516 with protection information");
	set_block_length(516, &r);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	rewind_tape();
	cdb6(0x08, 0x01, BUF_SZ / 516, data, BUF_SZ, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && r.data_len == (BUF_SZ / 516) * 516,
				"sam_stat %u data_len %u sense %x/%02x/%02x", r.sam_stat, r.data_len,
				r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	TEST_CHECK_(data[512] != 0 || data[513] != 0 || data[514] != 0 || data[515] != 0,
				"protection information after the first block");
	{
		int ok = 1;

		for (int j = 0; j < 512; j++)
			ok &= data[516 + j] == (uint8_t)((512 + j) * 31 + 1);
		TEST_CHECK_(ok, "second block at offset 516");
	}
	teardown();
}

TEST_LIST = {
	{"mode_select_block_descriptor", test_mode_select_block_descriptor},
	{"fixed_write_bounds", test_fixed_write_bounds},
	{"fixed_read_bounds", test_fixed_read_bounds},
	{"fixed_read_lbp_bounds", test_fixed_read_lbp_bounds},
	{NULL, NULL}};
