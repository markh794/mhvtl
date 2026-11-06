/*
 * test_attribute_bounds.c — READ ATTRIBUTE, READ MEDIA SERIAL NUMBER and
 * WRITE ATTRIBUTE stay within the 2 MiB command buffer.
 *
 *   - An ALLOCATION LENGTH of 2 GiB or more no longer turns negative when
 *     the buffer is cleared: the command completes and the drive answers.
 *   - READ MEDIA SERIAL NUMBER returns no more than the ALLOCATION LENGTH.
 *   - WRITE ATTRIBUTE (GA32-0928-07 §4.0.12, Tables 150-151, pp.176-177):
 *       - PARAMETER LIST LENGTH 0 is not an error and changes nothing;
 *       - a list larger than the buffer is refused, INVALID FIELD IN CDB on
 *         PARAMETER LIST LENGTH, and nothing is changed;
 *       - a list that truncates the PARAMETER DATA LENGTH field is a
 *         PARAMETER LIST LENGTH ERROR;
 *       - a short data-out transfer is ABORTED COMMAND, DATA PHASE ERROR;
 *       - a valid list is still written.
 *
 * Usage: ./test_attribute_bounds
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"

#define BUF_SZ		(2 * 1024 * 1024)
#define KEY_ILLEGAL 0x05
#define KEY_ABORTED 0x0b

static struct harness_daemon d;
static struct test_env		 env;
static uint8_t				 rbuf[4096];

static int setup(void) {
	const struct drive_type *dt;

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

static void read_attribute(uint16_t id, uint32_t alloc, struct scsi_result *r) {
	uint8_t cdb[16] = {0x8c, 0x00};

	put_unaligned_be16(id, &cdb[8]);
	put_unaligned_be32(alloc, &cdb[10]);
	memset(rbuf, 0xee, sizeof(rbuf));
	harness_send_cdb(&d, cdb, 16, r, rbuf, sizeof(rbuf));
}

static void write_attribute(uint32_t param_len, const uint8_t *data, uint32_t sent,
							struct scsi_result *r) {
	uint8_t cdb[16] = {0x8d, 0x00};

	put_unaligned_be32(param_len, &cdb[10]);
	harness_send_cdb_with_data_out(&d, cdb, 16, data, sent, r);
}

/* APPLICATION VENDOR (0800h), 8 ASCII bytes, as a WRITE ATTRIBUTE list */
static uint32_t vendor_list(uint8_t *p, const char *vendor) {
	memset(p, 0, 17);
	put_unaligned_be32(13, &p[0]);
	put_unaligned_be16(0x0800, &p[4]);
	p[6] = 0x01; /* ASCII */
	put_unaligned_be16(8, &p[7]);
	memcpy(&p[9], vendor, 8);
	return 17;
}

/* APPLICATION VENDOR as READ ATTRIBUTE returns it, or "" */
static const char *vendor(void) {
	static char		   v[9];
	struct scsi_result r;

	read_attribute(0x0800, sizeof(rbuf), &r);
	if (r.sam_stat != SAM_STAT_GOOD || get_unaligned_be16(&rbuf[4]) != 0x0800)
		return "";
	memcpy(v, &rbuf[9], 8);
	v[8] = 0;
	return v;
}

#define CHECK_SENSE(r, key, asc, ascq)                                               \
	TEST_CHECK_((r).sam_stat == SAM_STAT_CHECK_CONDITION &&                          \
					((r).sense[2] & 0x0f) == (key) && (r).sense[12] == (asc) &&      \
					(r).sense[13] == (ascq),                                         \
				"sam_stat %u sense %x/%02x/%02x", (r).sam_stat, (r).sense[2] & 0x0f, \
				(r).sense[12], (r).sense[13])

void test_read_allocation_length(void) {
	struct scsi_result r;

	TEST_ASSERT(setup() == 0);

	TEST_CASE("READ ATTRIBUTE, ALLOCATION LENGTH FFFFFFF0h");
	read_attribute(0x0000, 0xfffffff0, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "sam_stat %u sense %x/%02x/%02x", r.sam_stat,
				r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	TEST_CHECK_(alive(), "vtltape still answers");

	TEST_CASE("READ ATTRIBUTE, ALLOCATION LENGTH 80000000h");
	read_attribute(0x0000, 0x80000000, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "sam_stat %u", r.sam_stat);
	TEST_CHECK_(alive(), "vtltape still answers");

	TEST_CASE("READ MEDIA SERIAL NUMBER, ALLOCATION LENGTH 80000000h");
	{
		uint8_t cdb[12] = {0xab, 0x01};

		put_unaligned_be32(0x80000000, &cdb[6]);
		harness_send_cdb(&d, cdb, 12, &r, rbuf, sizeof(rbuf));
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "sam_stat %u", r.sam_stat);
		TEST_CHECK_(alive(), "vtltape still answers");
	}

	TEST_CASE("READ MEDIA SERIAL NUMBER, ALLOCATION LENGTH 4");
	{
		uint8_t cdb[12] = {0xab, 0x01};

		put_unaligned_be32(4, &cdb[6]);
		harness_send_cdb(&d, cdb, 12, &r, rbuf, sizeof(rbuf));
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD && r.data_len == 4, "sam_stat %u data_len %u",
					r.sam_stat, r.data_len);
	}
	teardown();
}

void test_write_attribute_bounds(void) {
	static uint8_t	   big[64 * 1024];
	uint8_t			   list[17];
	struct scsi_result r;
	uint32_t		   n;

	TEST_ASSERT(setup() == 0);
	n = vendor_list(list, "BEFORE  ");
	write_attribute(n, list, n, &r);
	TEST_ASSERT_(r.sam_stat == SAM_STAT_GOOD, "initial write (sense %x/%02x/%02x)",
				 r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	TEST_ASSERT(!strcmp(vendor(), "BEFORE  "));

	TEST_CASE("PARAMETER LIST LENGTH 0");
	write_attribute(0, list, 0, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "sam_stat %u", r.sam_stat);

	TEST_CASE("PARAMETER LIST LENGTH above the buffer");
	vendor_list(big, "AFTER   ");
	write_attribute(BUF_SZ + 1, big, sizeof(big), &r);
	CHECK_SENSE(r, KEY_ILLEGAL, 0x24, 0x00);
	TEST_CHECK_((r.sense[15] & 0xc0) == 0xc0 && get_unaligned_be16(&r.sense[16]) == 10,
				"SKSV/CD %02x field pointer %u", r.sense[15],
				get_unaligned_be16(&r.sense[16]));
	TEST_CHECK_(alive(), "vtltape still answers");

	TEST_CASE("PARAMETER LIST LENGTH FFFFFFFFh");
	write_attribute(0xffffffff, big, sizeof(big), &r);
	CHECK_SENSE(r, KEY_ILLEGAL, 0x24, 0x00);
	TEST_CHECK_(alive(), "vtltape still answers");

	TEST_CASE("PARAMETER DATA LENGTH truncated");
	write_attribute(2, list, 2, &r);
	CHECK_SENSE(r, KEY_ILLEGAL, 0x1a, 0x00);

	TEST_CASE("short data-out");
	n = vendor_list(list, "AFTER   ");
	write_attribute(64, list, n, &r);
	CHECK_SENSE(r, KEY_ABORTED, 0x4b, 0x00);
	TEST_CHECK_(alive(), "vtltape still answers");

	TEST_CASE("nothing changed");
	TEST_CHECK_(!strcmp(vendor(), "BEFORE  "), "vendor '%s'", vendor());

	TEST_CASE("a valid list is still written");
	write_attribute(n, list, n, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "sam_stat %u", r.sam_stat);
	TEST_CHECK_(!strcmp(vendor(), "AFTER   "), "vendor '%s'", vendor());
	teardown();
}

TEST_LIST = {
	{"read_allocation_length", test_read_allocation_length},
	{"write_attribute_bounds", test_write_attribute_bounds},
	{NULL, NULL}};
