/*
 * Reading an encrypted block must use the key the application loaded,
 * even after an unencrypted write has cleared app_encr_info.
 * With DECRYPTION MODE = DECRYPT, a filemark or end of data is reported as
 * such, not as unencrypted data.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"

static struct harness_daemon daemon_ctx;
static struct test_env		 test_env;

static const struct drive_type *drive(const char *name) {
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++)
		if (strcmp(dt->name, name) == 0)
			return dt;
	return NULL;
}

static int start_drive(const struct drive_type *dt) {
	struct scsi_result r;
	uint8_t			   tur[6] = {0x00};

	if (!dt || gen_test_env(&test_env, dt) < 0 ||
		harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env) < 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	if (harness_load_tape(TEST_MINOR_TAPE, dt->media_barcode) < 0)
		return -1;
	for (int i = 0; i < 100; i++) {
		harness_cdb(&daemon_ctx, tur, sizeof(tur), &r);
		if (r.sam_stat == SAM_STAT_GOOD)
			return 0;
		usleep(100000);
	}
	return -1;
}

static void stop_drive(void) {
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

/* SECURITY PROTOCOL OUT, Set Data Encryption page with a 32-byte key */
static int set_encryption(uint8_t encrypt_mode, uint8_t decrypt_mode, uint8_t key_base,
						  struct scsi_result *r) {
	uint8_t cdb[12] = {0xb5, 0x20, 0x00, 0x10};
	uint8_t page[52];

	memset(page, 0, sizeof(page));
	page[1]	 = 0x10;
	page[3]	 = sizeof(page) - 4;
	page[4]	 = 0x40; /* All I_T nexus */
	page[6]	 = encrypt_mode;
	page[7]	 = decrypt_mode;
	page[8]	 = 1;
	page[19] = 32;
	for (size_t i = 0; i < 32; i++)
		page[20 + i] = key_base + i;
	cdb[9] = sizeof(page);
	return harness_send_cdb_with_data_out(&daemon_ctx, cdb, sizeof(cdb), page, sizeof(page), r);
}

static void read_back_after_unencrypted_append(const char *name) {
	uint8_t			   block[512], buf[512];
	struct scsi_result r;

	TEST_CASE(name);
	TEST_ASSERT(start_drive(drive(name)) == 0);
	memset(block, 0x5a, sizeof(block));

	/* Block 0 encrypted */
	TEST_ASSERT(set_encryption(2, 2, 1, &r) == 0 && r.sam_stat == SAM_STAT_GOOD);
	TEST_ASSERT(harness_tape_write(&daemon_ctx, block, sizeof(block), &r) == 0);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);

	/* Block 1 unencrypted; the key stays loaded for decryption */
	TEST_ASSERT(set_encryption(0, 3, 1, &r) == 0 && r.sam_stat == SAM_STAT_GOOD);
	TEST_ASSERT(harness_tape_write(&daemon_ctx, block, sizeof(block), &r) == 0);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);

	TEST_ASSERT(harness_tape_rewind(&daemon_ctx, &r) == 0);
	TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "sam_stat %u sense %02x/%02x", r.sam_stat,
				r.sense[12], r.sense[13]);
	TEST_CHECK(memcmp(buf, block, sizeof(block)) == 0);

	/* Block 1 is unencrypted: mixed mode passes it through */
	TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "block 1 sam_stat %u", r.sam_stat);

	/* The key is still checked: another key reads block 0 as INCORRECT KEY */
	TEST_ASSERT(set_encryption(0, 3, 0x80, &r) == 0 && r.sam_stat == SAM_STAT_GOOD);
	TEST_ASSERT(harness_tape_rewind(&daemon_ctx, &r) == 0);
	TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION && (r.sense[2] & 0x0f) == DATA_PROTECT &&
					r.sense[12] == 0x74 && r.sense[13] == 0x03,
				"wrong key: sam_stat %u sense %02x %02x/%02x", r.sam_stat, r.sense[2], r.sense[12],
				r.sense[13]);
	stop_drive();
}

void test_lto_read_after_unencrypted_append(void) {
	read_back_after_unencrypted_append("LTO-8");
}

void test_t10k_read_after_unencrypted_append(void) {
	read_back_after_unencrypted_append("T10000C");
}

/* An encrypted tape with two files, read back with DECRYPTION MODE = DECRYPT */
static void read_across_filemark_in_decrypt_mode(const char *name) {
	uint8_t			   block[512], buf[512];
	struct scsi_result r;

	TEST_CASE(name);
	TEST_ASSERT(start_drive(drive(name)) == 0);
	TEST_ASSERT(set_encryption(2, 2, 1, &r) == 0 && r.sam_stat == SAM_STAT_GOOD);
	for (int i = 0; i < 2; i++) {
		memset(block, 0x30 + i, sizeof(block));
		TEST_ASSERT(harness_tape_write(&daemon_ctx, block, sizeof(block), &r) == 0);
		TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
		TEST_ASSERT(harness_tape_write_fm(&daemon_ctx, 1, &r) == 0);
		TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	}
	TEST_ASSERT(harness_tape_rewind(&daemon_ctx, &r) == 0);

	for (int i = 0; i < 2; i++) {
		TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
		memset(block, 0x30 + i, sizeof(block));
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && memcmp(buf, block, sizeof(block)) == 0,
					"file %d block: sam_stat %u sense %02x/%02x", i, r.sam_stat, r.sense[12],
					r.sense[13]);
		/* NO SENSE, FILEMARK, 00/01 */
		TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
		TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION && (r.sense[2] & 0x80) &&
						(r.sense[2] & 0x0f) == NO_SENSE && r.sense[12] == 0x00 &&
						r.sense[13] == 0x01,
					"file %d filemark: sam_stat %u sense %02x %02x/%02x", i, r.sam_stat,
					r.sense[2], r.sense[12], r.sense[13]);
	}
	/* BLANK CHECK, 00/05 end of data */
	TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION && (r.sense[2] & 0x0f) == BLANK_CHECK &&
					r.sense[12] == 0x00 && r.sense[13] == 0x05,
				"end of data: sam_stat %u sense %02x %02x/%02x", r.sam_stat, r.sense[2],
				r.sense[12], r.sense[13]);

	/* An unencrypted block is still refused in DECRYPT mode */
	TEST_ASSERT(set_encryption(0, 3, 1, &r) == 0 && r.sam_stat == SAM_STAT_GOOD);
	TEST_ASSERT(harness_tape_write(&daemon_ctx, block, sizeof(block), &r) == 0);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	TEST_ASSERT(set_encryption(0, 2, 1, &r) == 0 && r.sam_stat == SAM_STAT_GOOD);
	TEST_ASSERT(harness_tape_rewind(&daemon_ctx, &r) == 0);
	for (int i = 0; i < 4; i++)
		TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
	TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION && (r.sense[2] & 0x0f) == DATA_PROTECT &&
					r.sense[12] == 0x74 && r.sense[13] == 0x02,
				"unencrypted block: sam_stat %u sense %02x %02x/%02x", r.sam_stat, r.sense[2],
				r.sense[12], r.sense[13]);
	stop_drive();
}

void test_lto_read_across_filemark(void) {
	read_across_filemark_in_decrypt_mode("LTO-8");
}

void test_t10k_read_across_filemark(void) {
	read_across_filemark_in_decrypt_mode("T10000C");
}

TEST_LIST = {
	{"lto_read_after_unencrypted_append", test_lto_read_after_unencrypted_append},
	{"t10k_read_after_unencrypted_append", test_t10k_read_after_unencrypted_append},
	{"lto_read_across_filemark", test_lto_read_across_filemark},
	{"t10k_read_across_filemark", test_t10k_read_across_filemark},
	{NULL, NULL}};
