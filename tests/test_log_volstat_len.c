/*
 * Integration test: PAGE LENGTH of the LOG SENSE Volume Statistics page (17h).
 *
 * The page carries per-partition parameters (0200h-0204h) packed for the
 * number of partitions of the loaded cartridge, so it is shorter than the
 * template for fewer than MAX_PARTITIONS partitions. PAGE LENGTH must count
 * the bytes that follow the 4-byte page header (IBM LTO SCSI Reference
 * GA32-0928-07, 5.4.1, Table 234 "Log page layout", PAGE LENGTH (n-3), p.256;
 * https://www.ibm.com/support/pages/system/files/inline-files/%5BAPPROVED%5D%20LTO%20SCSI%20Reference%20GA32-0928-07%20%28EXTERNAL%29.pdf),
 * so PAGE LENGTH + 4 is the size of the page and walking the parameter list
 * ends exactly there.
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

#define VOLSTAT_PAGE 0x17

/* Partition record of 0200h-0204h: len, reserved, partition number (2), data (4) */
#define PARTITION_RECORD_SIZE4 (1 + 1 + 2 + 4)

static void log_sense_volstat(struct harness_daemon *d, uint16_t alloc,
			      struct scsi_result *r)
{
	uint8_t cdb[10] = { 0x4d, 0x00, 0x40 | VOLSTAT_PAGE, 0x00, 0x00, 0x00,
			    0x00, (uint8_t)(alloc >> 8), (uint8_t)alloc, 0x00 };

	memset(r, 0, sizeof(*r));
	TEST_CHECK(harness_cdb(d, cdb, 10, r) == 0);
}

/* Load the drive's default cartridge and wait until it is ready */
static int load_and_wait(struct harness_daemon *d, const struct drive_type *dt)
{
	struct scsi_result r;
	uint8_t tur[6] = { 0x00 };

	if (harness_load_tape(TEST_MINOR_TAPE, dt->media_barcode) != 0)
		return 0;
	for (int i = 0; i < 100; i++) {
		harness_cdb(d, tur, 6, &r);
		if (r.sam_stat == SAM_STAT_GOOD)
			return 1;
		usleep(100000);
	}
	return 0;
}

/* Partition the loaded cartridge: MODE SELECT(6) of the medium partition page
 * (11h) asking for n partitions, then FORMAT MEDIUM (04h) with FORMAT=1 */
static int partition_tape(struct harness_daemon *d, int n)
{
	uint8_t pl[4 + 16] = { 0 };
	uint8_t sel[6] = { 0x15, 0x10, 0x00, 0x00, sizeof(pl), 0x00 };
	uint8_t fmt[6] = { 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 };
	struct scsi_result r;

	pl[4] = 0x11;		/* page code */
	pl[5] = 14;		/* page length */
	pl[6] = 3;		/* maximum additional partitions */
	pl[7] = n - 1;		/* additional partitions defined */
	pl[8] = 0x9c;		/* FDP | PSUM | POFM */
	pl[9] = 0x03;		/* medium format recognition */
	pl[10] = 0x09;		/* partitioning type | partition units */
	/* FDP set: the drive picks the sizes, the descriptors stay zero */
	memset(&r, 0, sizeof(r));
	if (harness_send_cdb_with_data_out(d, sel, 6, pl, sizeof(pl), &r) < 0 ||
	    r.sam_stat != SAM_STAT_GOOD)
		return -1;
	memset(&r, 0, sizeof(r));
	if (harness_cdb(d, fmt, 6, &r) < 0 || r.sam_stat != SAM_STAT_GOOD)
		return -1;
	return 0;
}

/* Walk the parameter list; returns the parameter count, -1 if it overruns */
static int walk_params(const uint8_t *page, size_t end, uint16_t *codes, int max)
{
	size_t pos = 4;
	int n = 0;

	while (pos < end) {
		if (pos + 4 > end || n >= max)
			return -1;
		codes[n++] = (page[pos] << 8) | page[pos + 1];
		pos += 4 + page[pos + 3];
	}
	return pos == end ? n : -1;
}

/* partitions < 0: leave the drive empty */
static void check_page(const char *drive, int partitions)
{
	const struct drive_type *dt = NULL;
	struct test_env env;
	struct harness_daemon d;
	struct scsi_result r;
	uint16_t codes[64];
	size_t page_len;
	int n;

	for (const struct drive_type *t = ALL_DRIVES; t->name; t++)
		if (!strcmp(t->name, drive))
			dt = t;
	TEST_ASSERT(dt != NULL);

	memset(&env, 0, sizeof(env));
	TEST_ASSERT_(gen_test_env(&env, dt) == 0, "gen_test_env %s", drive);
	if (harness_start_env(&d, TEST_MINOR_TAPE, "tape", &env) < 0) {
		TEST_CHECK_(0, "vtltape did not start");
		harness_stop(&d);
		gen_cleanup_env(&env);
		return;
	}
	harness_clear_ua(&d);

	if (partitions >= 0)
		TEST_ASSERT_(load_and_wait(&d, dt), "%s: cartridge loaded and ready", drive);

	if (partitions > 1)
		TEST_ASSERT_(partition_tape(&d, partitions) == 0,
			     "%s: partitioning to %d", drive, partitions);

	log_sense_volstat(&d, 4096, &r);
	TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "%s: LOG SENSE 17h sam_stat=%d",
		    drive, r.sam_stat);
	TEST_ASSERT_(r.data_len >= 4, "%s: data_len=%u", drive, r.data_len);

	page_len = (r.data[2] << 8) | r.data[3];
	TEST_CHECK_(page_len + 4 == r.data_len,
		    "%s: PAGE LENGTH %zu + 4 == bytes returned %u",
		    drive, page_len, r.data_len);

	n = walk_params(r.data, r.data_len, codes, 64);
	TEST_CHECK_(n > 0, "%s: parameter list ends exactly at the page end", drive);

	/* One record per partition in each of 0200h-0204h: the 0202h
	 * parameter length is partitions * sizeof(a size 4 record) */
	if (n > 0 && partitions >= 0) {
		size_t pos = 4;

		while (pos < r.data_len && ((r.data[pos] << 8) | r.data[pos + 1]) != 0x0202)
			pos += 4 + r.data[pos + 3];
		TEST_CHECK_(pos < r.data_len && r.data[pos + 3] == partitions * PARTITION_RECORD_SIZE4,
			    "%s: 0202h length %u, expected %d partition(s) x %d",
			    drive, pos < r.data_len ? r.data[pos + 3] : 0, partitions,
			    PARTITION_RECORD_SIZE4);
	}

	harness_stop(&d);
	gen_cleanup_env(&env);
}

void test_volstat_len_1_partition(void)
{
	check_page("LTO-1", 1);
}

void test_volstat_len_2_partitions(void)
{
	check_page("LTO-5", 2);
}

void test_volstat_len_4_partitions(void)
{
	check_page("LTO-5", 4);
}

void test_volstat_len_no_tape(void)
{
	check_page("LTO-5", -1);
}

TEST_LIST = {
	{ "volstat_len_1_partition", test_volstat_len_1_partition },
	{ "volstat_len_2_partitions", test_volstat_len_2_partitions },
	{ "volstat_len_4_partitions", test_volstat_len_4_partitions },
	{ "volstat_len_no_tape", test_volstat_len_no_tape },
	{ NULL, NULL }
};
