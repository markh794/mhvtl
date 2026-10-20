/*
 * SECURITY PROTOCOL OUT must atomically install logical encryption state,
 * and CKOD must clear it when the volume is demounted.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "be_byteshift.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"

static struct harness_daemon daemon_ctx;
static struct test_env		 test_env;

static const struct drive_type *lto8(void) {
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++)
		if (strcmp(dt->name, "LTO-8") == 0)
			return dt;
	return NULL;
}

static int start_drive(void) {
	const struct drive_type *dt = lto8();
	struct scsi_result		 r;
	uint8_t					 tur[6] = {0x00};

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

static int spin(uint16_t page, uint8_t *buf, size_t buf_len, struct scsi_result *r) {
	uint8_t cdb[12] = {0xa2, 0x20};

	cdb[2] = page >> 8;
	cdb[3] = page;
	cdb[6] = buf_len >> 24;
	cdb[7] = buf_len >> 16;
	cdb[8] = buf_len >> 8;
	cdb[9] = buf_len;
	return harness_send_cdb(&daemon_ctx, cdb, sizeof(cdb), r, buf, buf_len);
}

static int spout(uint8_t *data, size_t len, struct scsi_result *r) {
	uint8_t cdb[12] = {0xb5, 0x20, 0x00, 0x10};

	cdb[6] = len >> 24;
	cdb[7] = len >> 16;
	cdb[8] = len >> 8;
	cdb[9] = len;
	return harness_send_cdb_with_data_out(&daemon_ctx, cdb, sizeof(cdb),
										  data, len, r);
}

static void set_page(uint8_t *data, size_t len, int ckod) {
	memset(data, 0, len);
	data[1]	 = 0x10;
	data[2]	 = (len - 4) >> 8;
	data[3]	 = len - 4;
	data[4]	 = 0x40; /* All I_T nexus */
	data[5]	 = ckod ? 0x04 : 0;
	data[6]	 = 2;
	data[7]	 = 2;
	data[8]	 = 1;
	data[18] = 0;
	data[19] = 32;
	for (size_t i = 0; i < 32; i++)
		data[20 + i] = i + 1;
}

void test_capability_and_status_scope(void) {
	uint8_t			   buf[64] = {0};
	struct scsi_result r;

	TEST_ASSERT(start_drive() == 0);
	TEST_ASSERT(spin(0x0012, buf, sizeof(buf), &r) == 0);
	TEST_CHECK(r.sam_stat == SAM_STAT_GOOD);
	TEST_CHECK_(buf[4] == 0 && buf[5] == 0x04 && buf[7] == 0x04,
				"management capabilities %02x/%02x/%02x", buf[4], buf[5], buf[7]);
	TEST_ASSERT(spin(0x0020, buf, sizeof(buf), &r) == 0);
	TEST_CHECK(r.sam_stat == SAM_STAT_GOOD);
	TEST_CHECK_(buf[4] == 0x42, "status scope %02x", buf[4]);
	stop_drive();
}

void test_rejected_spout_preserves_state(void) {
	uint8_t			   page[89], before[64] = {0}, after[64] = {0};
	struct scsi_result r;

	TEST_ASSERT(start_drive() == 0);
	set_page(page, 52, 0);
	memset(page + 52, 0, sizeof(page) - 52);
	TEST_ASSERT(spout(page, 52, &r) == 0);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	TEST_ASSERT(spin(0x0020, before, sizeof(before), &r) == 0);
	page[3]	 = 85;
	page[52] = 0;
	page[54] = 0;
	page[55] = 33;
	TEST_ASSERT(spout(page, sizeof(page), &r) == 0);
	TEST_CHECK(r.sam_stat == SAM_STAT_CHECK_CONDITION);
	TEST_ASSERT(spin(0x0020, after, sizeof(after), &r) == 0);
	TEST_CHECK(memcmp(before, after, 24) == 0);
	stop_drive();
}

void test_ckod_clears_state_on_unload(void) {
	uint8_t			   page[52], status[64] = {0};
	struct scsi_result r;
	uint32_t		   before;

	TEST_ASSERT(start_drive() == 0);
	set_page(page, sizeof(page), 1);
	TEST_ASSERT(spout(page, sizeof(page), &r) == 0);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	TEST_ASSERT(spin(0x0020, status, sizeof(status), &r) == 0);
	before = get_unaligned_be32(&status[8]);
	TEST_ASSERT(harness_unload_tape(TEST_MINOR_TAPE) == 0);
	TEST_ASSERT(spin(0x0020, status, sizeof(status), &r) == 0);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && status[5] == 0 && status[6] == 0,
				"status after CKOD unload: mode %u/%u", status[5], status[6]);
	/* The key instance counter keeps counting: the parameters changed */
	TEST_CHECK_(get_unaligned_be32(&status[8]) == before + 1, "counter %u -> %u", before,
				get_unaligned_be32(&status[8]));
	stop_drive();
}

static int is_invalid_field_in_parms(const struct scsi_result *r) {
	return r->sam_stat == SAM_STAT_CHECK_CONDITION &&
		   (r->sense[2] & 0x0f) == ILLEGAL_REQUEST &&
		   r->sense[12] == 0x26 && r->sense[13] == 0x00;
}

/* stenc sends CEEM = 01b and may set RDMC; both must still be accepted */
void test_ceem_and_rdmc_accepted(void) {
	uint8_t			   page[52];
	struct scsi_result r;

	TEST_ASSERT(start_drive() == 0);
	set_page(page, sizeof(page), 0);
	page[5] = 0x40 | 0x20; /* CEEM 01b, RDMC 10b */
	TEST_ASSERT(spout(page, sizeof(page), &r) == 0);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "sam_stat %u", r.sam_stat);
	stop_drive();
}

/* Unadvertised scope, LOCK, CKORP/CKORL, and CKOD with no medium are refused */
void test_unsupported_controls_rejected(void) {
	static const struct {
		const char *name;
		uint8_t		byte4;
		uint8_t		byte5;
		int			unload;
	} cases[] = {
		{"public scope", 0x00, 0x00, 0},
		{"local scope", 0x20, 0x00, 0},
		{"lock", 0x41, 0x00, 0},
		{"ckorp", 0x40, 0x02, 0},
		{"ckorl", 0x40, 0x01, 0},
		{"ckod without medium", 0x40, 0x04, 1},
	};
	uint8_t			   page[52];
	struct scsi_result r;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		TEST_CASE(cases[i].name);
		TEST_ASSERT(start_drive() == 0);
		if (cases[i].unload)
			TEST_ASSERT(harness_unload_tape(TEST_MINOR_TAPE) == 0);
		set_page(page, sizeof(page), 0);
		page[4] = cases[i].byte4;
		page[5] = cases[i].byte5;
		TEST_ASSERT(spout(page, sizeof(page), &r) == 0);
		TEST_CHECK_(is_invalid_field_in_parms(&r) && r.sense[15] & 0x80 &&
						r.sense[17] == (cases[i].byte5 ? 5 : 4),
					"sam_stat %u sense %02x/%02x fp %u", r.sam_stat, r.sense[12],
					r.sense[13], r.sense[17]);
		stop_drive();
	}
}

/* Without CKOD the key survives a demount */
void test_no_ckod_keeps_state_on_unload(void) {
	uint8_t			   page[52], status[64] = {0};
	struct scsi_result r;

	TEST_ASSERT(start_drive() == 0);
	set_page(page, sizeof(page), 0);
	TEST_ASSERT(spout(page, sizeof(page), &r) == 0);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);
	TEST_ASSERT(harness_unload_tape(TEST_MINOR_TAPE) == 0);
	TEST_ASSERT(spin(0x0020, status, sizeof(status), &r) == 0);
	TEST_CHECK_(status[5] == 2 && status[6] == 2, "mode %u/%u", status[5], status[6]);
	stop_drive();
}

static unsigned int field_pointer(const struct scsi_result *r) {
	return (r->sense[15] & 0x80) ? (r->sense[16] << 8) | r->sense[17] : 0xffff;
}

/* Malformed KAD lists and a missing key are refused with 26h/00h */
void test_malformed_pages_rejected(void) {
	uint8_t			   page[64];
	struct scsi_result r;

	TEST_ASSERT(start_drive() == 0);

	TEST_CASE("duplicate U-KAD");
	set_page(page, 60, 0);
	page[52] = 0x00, page[55] = 0; /* U-KAD, length 0 */
	page[56] = 0x00, page[59] = 0; /* U-KAD again */
	TEST_ASSERT(spout(page, 60, &r) == 0);
	TEST_CHECK(is_invalid_field_in_parms(&r) && field_pointer(&r) == 56);

	TEST_CASE("nonce descriptor");
	set_page(page, 56, 0);
	page[52] = 0x02;
	TEST_ASSERT(spout(page, 56, &r) == 0);
	TEST_CHECK(is_invalid_field_in_parms(&r) && field_pointer(&r) == 52);

	TEST_CASE("AUTHENTICATED set");
	set_page(page, 56, 0);
	page[53] = 0x01;
	TEST_ASSERT(spout(page, 56, &r) == 0);
	TEST_CHECK(is_invalid_field_in_parms(&r) && field_pointer(&r) == 53);

	TEST_CASE("KAD longer than 32 bytes");
	memset(page, 0, sizeof(page));
	set_page(page, 56, 0);
	page[55] = 33;
	TEST_ASSERT(spout(page, 56, &r) == 0);
	TEST_CHECK(is_invalid_field_in_parms(&r) && field_pointer(&r) == 54);

	TEST_CASE("truncated descriptor");
	set_page(page, 54, 0);
	TEST_ASSERT(spout(page, 54, &r) == 0);
	TEST_CHECK(is_invalid_field_in_parms(&r) && field_pointer(&r) == 52);

	TEST_CASE("16-byte key");
	set_page(page, 36, 0);
	page[19] = 16;
	TEST_ASSERT(spout(page, 36, &r) == 0);
	TEST_CHECK(is_invalid_field_in_parms(&r));

	TEST_CASE("encrypt without key");
	set_page(page, 20, 0);
	page[19] = 0;
	TEST_ASSERT(spout(page, 20, &r) == 0);
	TEST_CHECK(is_invalid_field_in_parms(&r) && field_pointer(&r) == 18);

	TEST_CASE("decrypt without key");
	set_page(page, 20, 0);
	page[6]	 = 0;
	page[7]	 = 3;
	page[19] = 0;
	TEST_ASSERT(spout(page, 20, &r) == 0);
	TEST_CHECK(is_invalid_field_in_parms(&r) && field_pointer(&r) == 18);

	TEST_CASE("raw decrypt needs no key");
	set_page(page, 20, 0);
	page[6]	 = 0;
	page[7]	 = 1;
	page[19] = 0;
	TEST_ASSERT(spout(page, 20, &r) == 0);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "sam_stat %u", r.sam_stat);

	TEST_CASE("page length beyond transfer");
	set_page(page, 52, 0);
	page[3] = 60;
	TEST_ASSERT(spout(page, 52, &r) == 0);
	TEST_CHECK(is_invalid_field_in_parms(&r));

	stop_drive();
}

/* A transfer length larger than the data buffer must not reach resp_spout() */
void test_oversized_transfer_length_rejected(void) {
	uint8_t			   cdb[12] = {0xb5, 0x20, 0x00, 0x10};
	uint8_t			   page[52];
	uint8_t			   tur[6] = {0x00};
	struct scsi_result r;

	TEST_ASSERT(start_drive() == 0);
	set_page(page, sizeof(page), 0);
	cdb[6] = 0x10; /* 256 MiB */
	TEST_ASSERT(harness_send_cdb_with_data_out(&daemon_ctx, cdb, sizeof(cdb),
											   page, sizeof(page), &r) == 0);
	TEST_CHECK_(r.sam_stat == SAM_STAT_CHECK_CONDITION &&
					(r.sense[2] & 0x0f) == ILLEGAL_REQUEST && r.sense[12] == 0x24,
				"sam_stat %u sense %02x/%02x", r.sam_stat, r.sense[2], r.sense[12]);
	TEST_CHECK(harness_cdb(&daemon_ctx, tur, sizeof(tur), &r) == 0);
	stop_drive();
}

TEST_LIST = {
	{"capability_and_status_scope", test_capability_and_status_scope},
	{"rejected_spout_preserves_state", test_rejected_spout_preserves_state},
	{"ckod_clears_state_on_unload", test_ckod_clears_state_on_unload},
	{"ceem_and_rdmc_accepted", test_ceem_and_rdmc_accepted},
	{"unsupported_controls_rejected", test_unsupported_controls_rejected},
	{"no_ckod_keeps_state_on_unload", test_no_ckod_keeps_state_on_unload},
	{"malformed_pages_rejected", test_malformed_pages_rejected},
	{"oversized_transfer_length_rejected", test_oversized_transfer_length_rejected},
	{NULL, NULL}};
