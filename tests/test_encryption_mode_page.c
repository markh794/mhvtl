/*
 * Integration tests for the encryption bits of the IBM vendor-specific
 * mode page 24h.
 *
 * IBM LTO SCSI Reference GA32-0928-09, 6.6.19 "MP 24h: Vendor-Specific",
 * table 356: byte 7 carries ENCR_C in bit 0 and ENCR_E in bit 3. Bytes 4
 * to 6 are reserved.
 *
 * mhVTL sets ENCR_E when SECURITY PROTOCOL OUT turns encryption on and
 * clears it when encryption is turned off.
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

#define MP24_ENCR_C 0x01 /* byte 7, bit 0 */
#define MP24_ENCR_E 0x08 /* byte 7, bit 3 */

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

static const struct drive_type *find_drive(const char *name)
{
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++)
		if (!strcmp(dt->name, name))
			return dt;
	return NULL;
}

static int start(const char *drive)
{
	const struct drive_type *dt = find_drive(drive);

	if (!dt)
		return -1;
	gen_cleanup_env(&test_env);
	if (gen_test_env(&test_env, dt) < 0)
		return -1;
	if (harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env) < 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	return 0;
}

static void stop(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

/*
 * MODE SENSE(6), DBD=1, page 24h. Copies the eight bytes of the page into
 * page[]. Returns 0 on success.
 */
static int read_mp24(uint8_t *page)
{
	uint8_t cdb[6] = { 0x1a, 0x08, 0x24, 0x00, 0xff, 0x00 };
	struct scsi_result r;
	unsigned int off;

	memset(&r, 0, sizeof(r));
	if (harness_cdb(&daemon_ctx, cdb, sizeof(cdb), &r) < 0)
		return -1;
	if (!TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
			 "MODE SENSE(6) page 24h sam_stat=%d sense %02x/%02x/%02x",
			 r.sam_stat, r.sense[2] & 0x0f, r.sense[12], r.sense[13]))
		return -1;

	/* 4-byte header, then any block descriptors, then the page */
	off = 4 + r.data[3];
	if (!TEST_CHECK_(r.data_len >= off + 8 && r.data[off] == 0x24 &&
			 r.data[off + 1] == 0x06,
			 "page 24h present (len %u, code %02x, page length %02x)",
			 r.data_len, r.data[off], r.data[off + 1]))
		return -1;
	memcpy(page, &r.data[off], 8);
	return 0;
}

/*
 * SECURITY PROTOCOL OUT, Tape Data Encryption, Set Data Encryption page.
 * encrypt != 0 selects ENCRYPT/DECRYPT with a 256-bit key; otherwise
 * DISABLE/DISABLE with no key.
 */
static int set_encryption(int encrypt)
{
	uint8_t page[20 + 32];
	uint32_t len = encrypt ? sizeof(page) : 20;
	uint8_t cdb[12] = { 0xb5, 0x20, 0x00, 0x10, 0x00, 0x00,
			    (uint8_t)(len >> 24), (uint8_t)(len >> 16),
			    (uint8_t)(len >> 8), (uint8_t)len, 0x00, 0x00 };
	struct scsi_result r;

	memset(page, 0, sizeof(page));
	page[1] = 0x10;			/* PAGE CODE 0010h */
	page[3] = (uint8_t)(len - 4);	/* PAGE LENGTH */
	page[4] = 0x40;			/* SCOPE: all I_T nexus */
	page[6] = encrypt ? 0x02 : 0x00; /* ENCRYPTION MODE: ENCRYPT / DISABLE */
	page[7] = encrypt ? 0x02 : 0x00; /* DECRYPTION MODE: DECRYPT / DISABLE */
	page[8] = 0x01;			/* ALGORITHM INDEX */
	page[9] = 0x00;			/* KEY FORMAT: plain text */
	if (encrypt) {
		page[19] = 0x20;	/* KEY LENGTH 32 */
		for (int i = 0; i < 32; i++)
			page[20 + i] = (uint8_t)(0xa0 + i);
	}

	memset(&r, 0, sizeof(r));
	if (harness_send_cdb_with_data_out(&daemon_ctx, cdb, sizeof(cdb),
					   page, len, &r) < 0)
		return -1;
	if (!TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD,
			 "SPOUT set data encryption (%s) sam_stat=%d sense %02x/%02x/%02x",
			 encrypt ? "on" : "off", r.sam_stat,
			 r.sense[2] & 0x0f, r.sense[12], r.sense[13]))
		return -1;
	return 0;
}

static void check_mp24(const char *what, uint8_t expect_byte7)
{
	uint8_t page[8];

	if (read_mp24(page) < 0)
		return;
	TEST_CHECK_(page[4] == 0 && page[5] == 0 && page[6] == 0,
		    "%s: reserved bytes 4-6 are zero (got %02x %02x %02x)",
		    what, page[4], page[5], page[6]);
	TEST_CHECK_(page[7] == expect_byte7,
		    "%s: byte 7 = %02x (expected %02x)",
		    what, page[7], expect_byte7);
}

void test_ibm_lto_encr_e(void)
{
	const char *drives[] = { "LTO-5", "LTO-8", NULL };

	for (int i = 0; drives[i]; i++) {
		TEST_CASE(drives[i]);
		if (start(drives[i]) < 0) {
			TEST_CHECK_(0, "daemon start for %s", drives[i]);
			stop();
			continue;
		}
		check_mp24("power-on", MP24_ENCR_C);

		if (set_encryption(1) == 0)
			check_mp24("encryption on", MP24_ENCR_C | MP24_ENCR_E);

		if (set_encryption(0) == 0)
			check_mp24("encryption off", MP24_ENCR_C);

		stop();
	}
}

TEST_LIST = {
	{ "ibm_lto_encr_e", test_ibm_lto_encr_e },
	{ NULL, NULL }
};
