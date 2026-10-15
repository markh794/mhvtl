/*
 * Integration tests for READ ELEMENT STATUS device identifiers (DVCID)
 * and personality selection - markh794/mhvtl#152.
 *
 * Starts vtllibrary (and vtltape) with MHVTL_BACKEND=userland against a
 * generated device.conf, then decodes the data transfer element page.
 *
 * Reference for the Scalar layout: Quantum Scalar Intelligent Libraries
 * SCSI Reference Guide 6-00423-16 Rev B, Table 75.
 */
#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define LIB_MINOR  10
#define TAPE_MINOR 11

#define LONG_SERIAL "QTMSN0123456789ABCDEFGHIJ" /* 25 characters */

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

struct lib_conf {
	const char *vendor;
	const char *product;
	const char *drive_vendor;
	const char *drive_product;
	const char *drive_serial;
	int extra_drive; /* add a Drive 3 position with no drive configured */
};

static int prepare_env(const struct lib_conf *c)
{
	char path[1024];
	char mktape_path[256];
	FILE *fp;
	const char *barcode = "E01001L8";

	memset(&test_env, 0, sizeof(test_env));
	snprintf(test_env.base_dir, sizeof(test_env.base_dir),
		 "/tmp/mhvtl-dvcid-XXXXXX");
	if (!mkdtemp(test_env.base_dir))
		return -1;

	snprintf(test_env.conf_dir, sizeof(test_env.conf_dir), "%s/conf",
		 test_env.base_dir);
	snprintf(test_env.home_dir, sizeof(test_env.home_dir), "%s/home",
		 test_env.base_dir);
	snprintf(test_env.lock_dir, sizeof(test_env.lock_dir), "%s/lock",
		 test_env.base_dir);
	snprintf(test_env.run_dir, sizeof(test_env.run_dir), "%s/run",
		 test_env.base_dir);

	mkdir(test_env.conf_dir, 0755);
	mkdir(test_env.home_dir, 0755);
	mkdir(test_env.lock_dir, 0755);
	mkdir(test_env.run_dir, 0755);

	snprintf(path, sizeof(path), "%s/mhvtl.conf", test_env.conf_dir);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp, "MHVTL_CONFIG_PATH=%s\nMHVTL_HOME_PATH=%s\n",
		test_env.conf_dir, test_env.home_dir);
	fclose(fp);

	snprintf(path, sizeof(path), "%s/device.conf", test_env.conf_dir);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp,
		"VERSION: 5\n"
		"\n"
		"Library: %u CHANNEL: 00 TARGET: 00 LUN: 00\n"
		" Vendor identification: %s\n"
		" Product identification: %s\n"
		" Product revision level: 0100\n"
		" Unit serial number: TESTLIB00\n"
		" NAA: 50:11:22:33:44:55:66:00\n"
		" Home directory: %s\n"
		"\n"
		"Drive: %u CHANNEL: 00 TARGET: 01 LUN: 00\n"
		" Library ID: %u Slot: 1\n"
		" Vendor identification: %s\n"
		" Product identification: %s\n"
		" Product revision level: 0100\n"
		" Unit serial number: %s\n"
		" NAA: 50:11:22:33:44:55:66:01\n"
		"\n"
		"Drive: %u CHANNEL: 00 TARGET: 02 LUN: 00\n"
		" Library ID: %u Slot: 2\n"
		" Vendor identification: IBM\n"
		" Product identification: ULT3580-TD8\n"
		" Product revision level: 0100\n"
		" Unit serial number: XYZZY_B\n"
		" NAA: 50:11:22:33:44:55:66:02\n"
		"\n",
		LIB_MINOR, c->vendor, c->product, test_env.home_dir,
		TAPE_MINOR, LIB_MINOR,
		c->drive_vendor, c->drive_product, c->drive_serial,
		TAPE_MINOR + 1, LIB_MINOR);
	fclose(fp);

	snprintf(path, sizeof(path), "%s/library_contents.%u",
		 test_env.conf_dir, LIB_MINOR);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp, "Drive 1:\nDrive 2:\n%sPicker 1:\nMAP 1:\nSlot 1: %s\nSlot 2:\n",
		c->extra_drive ? "Drive 3:\n" : "", barcode);
	fclose(fp);

	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;
	snprintf(path, sizeof(path),
		 "%s%s -C %s -H %s -l %u -m %s -s 500 -t data -d LTO8 >/dev/null 2>&1",
		 gen_local_lib_prefix(mktape_path), mktape_path,
		 test_env.conf_dir, test_env.home_dir, LIB_MINOR, barcode);
	return system(path);
}

static int start(const struct lib_conf *c, unsigned minor, const char *type)
{
	gen_cleanup_env(&test_env);
	if (prepare_env(c) != 0)
		return -1;
	if (harness_start_env(&daemon_ctx, minor, type, &test_env) != 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	return 0;
}

static void stop(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

static uint8_t res_buf[4096];

/* READ ELEMENT STATUS for all element types. Returns bytes received. */
static int read_element_status(int voltag, int dvcid)
{
	uint8_t cdb[12] = {
		0xb8, voltag ? 0x10 : 0x00,
		0x00, 0x00,		/* starting element */
		0xff, 0xff,		/* number of elements */
		dvcid ? 0x01 : 0x00,
		0x00, 0x0f, 0xe8,	/* allocation length 4072 */
		0x00, 0x00
	};
	struct scsi_result r;

	memset(res_buf, 0, sizeof(res_buf));
	if (harness_send_cdb(&daemon_ctx, cdb, 12, &r, res_buf,
			     sizeof(res_buf)) != 0)
		return -1;
	if (r.sam_stat != SAM_STAT_GOOD)
		return -1;
	return (int)r.data_len;
}

struct elem_page {
	int desc_len;
	int byte_count;
	uint8_t *desc; /* first descriptor */
};

/* Walk the element status pages; return 0 if a page of 'type' is found. */
static int find_page(int len, uint8_t type, struct elem_page *pg)
{
	int total = get_unaligned_be24(&res_buf[5]);
	uint8_t *p = &res_buf[8];
	uint8_t *end = &res_buf[8] + total;

	if (len < 8 || total + 8 > len)
		return -1;

	while (p + 8 <= end) {
		int dl = get_unaligned_be16(&p[2]);
		int bc = get_unaligned_be24(&p[5]);

		if (p[0] == type) {
			pg->desc_len = dl;
			pg->byte_count = bc;
			pg->desc = p + 8;
			return 0;
		}
		p += 8 + bc;
	}
	return -1;
}

/* Offset of the identification descriptor header in a DTE descriptor */
static int id_offset(int voltag)
{
	return 12 + (voltag ? 36 : 0);
}

static const struct lib_conf scalar_conf = {
	"QUANTUM", "Scalar i6000", "IBM", "ULTRIUM-TD7", "F0027BE0F1"
};

void test_scalar_selected_by_vendor(void)
{
	static const char *vendors[] = { "QUANTUM", "ADIC" };

	for (unsigned i = 0; i < ARRAY_SIZE(vendors); i++) {
		struct lib_conf c = scalar_conf;
		struct elem_page pg;
		int len;

		c.vendor = vendors[i];
		TEST_CASE(vendors[i]);
		TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);

		len = read_element_status(0, 0);
		TEST_CHECK(len > 8);
		TEST_CHECK(find_page(len, 4, &pg) == 0);
		TEST_CHECK(get_unaligned_be16(&pg.desc[0]) == 0x0100);
		TEST_MSG("first DTE address 0x%04x",
			 get_unaligned_be16(&pg.desc[0]));
		TEST_CHECK(find_page(len, 2, &pg) == 0);
		TEST_CHECK(get_unaligned_be16(&pg.desc[0]) == 0x1000);
		TEST_MSG("first storage address 0x%04x",
			 get_unaligned_be16(&pg.desc[0]));

		stop();
	}
}

void test_default_personality_consistent(void)
{
	struct lib_conf c = {
		"ACME", "VLIB 9000", "IBM", "ULT3580-TD8", "XYZZY_A"
	};

	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);

	for (int voltag = 0; voltag <= 1; voltag++) {
		struct elem_page pg;
		uint8_t *id;
		int len = read_element_status(voltag, 1);

		TEST_CASE_("voltag=%d", voltag);
		TEST_CHECK(len > 8);
		TEST_ASSERT(find_page(len, 4, &pg) == 0);
		/* Two drives configured */
		TEST_CHECK(pg.byte_count == 2 * pg.desc_len);
		/* The advertised length covers the whole identifier */
		id = &pg.desc[id_offset(voltag)];
		TEST_CHECK(id[3] == pg.desc_len - id_offset(voltag) - 4);
		TEST_CHECK(id[3] == 34);
		TEST_CHECK(memcmp(&id[4],
				  "IBM     ULT3580-TD8     000XYZZY_A", 34) == 0);
		/* The second descriptor starts where the length says */
		id = &pg.desc[pg.desc_len + id_offset(voltag)];
		TEST_CHECK(get_unaligned_be16(&pg.desc[pg.desc_len]) ==
			   get_unaligned_be16(&pg.desc[0]) + 1);
		TEST_CHECK(memcmp(&id[4],
				  "IBM     ULT3580-TD8     000XYZZY_B", 34) == 0);
		TEST_MSG("desc_len %d, byte_count %d, id_len %d",
			 pg.desc_len, pg.byte_count, id[3]);
		/* Header byte count covers exactly what was sent */
		TEST_CHECK(get_unaligned_be24(&res_buf[5]) + 8 == len);
	}

	stop();
}

/* IBM TS3100/TS3200 (3573): code set 2, type 1, 34 byte identifier,
 * GA32-0547-02 p. 3-68. The 7 character serial of the IBM LTO drive is
 * reported zero-padded to 10 characters, as the drive does.
 */
void test_ibm_layout_unchanged(void)
{
	struct lib_conf c = {
		"IBM", "3573-TL", "IBM", "ULT3580-TD8", "XYZZY_A"
	};
	struct elem_page pg;
	uint8_t *id;
	int len;

	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);

	len = read_element_status(0, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 50);
	id = &pg.desc[id_offset(0)];
	TEST_CHECK(id[0] == 2 && id[1] == 1 && id[3] == 34);
	TEST_CHECK(memcmp(&id[4],
			  "IBM     ULT3580-TD8     000XYZZY_A", 34) == 0);
	TEST_MSG("identifier: '%.34s'", &id[4]);

	stop();
}

void test_scalar_dvcid_area(void)
{
	struct elem_page pg;
	uint8_t *id;
	int len;

	TEST_ASSERT(start(&scalar_conf, LIB_MINOR, "library") == 0);

	len = read_element_status(1, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 116);
	TEST_MSG("voltag=1 desc_len %d", pg.desc_len);

	len = read_element_status(0, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 80);
	TEST_MSG("voltag=0 desc_len %d", pg.desc_len);
	id = &pg.desc[id_offset(0)];
	TEST_CHECK(id[3] == 34);
	TEST_CHECK(memcmp(&id[4],
			  "IBM     ULTRIUM-TD7     F0027BE0F1", 34) == 0);
	for (int i = 34; i < 64; i++)
		TEST_CHECK_(id[4 + i] == ' ', "pad byte %d is space", i);

	/* Without DVCID the identifier area is omitted */
	len = read_element_status(0, 0);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 16);

	stop();
}

static const struct lib_conf scalar_long_conf = {
	"QUANTUM", "Scalar i6000", "IBM", "ULTRIUM-TD7", LONG_SERIAL
};

static const char long_identifier[] =
	"IBM     ULTRIUM-TD7     " LONG_SERIAL;

void test_scalar_long_serial(void)
{
	struct elem_page pg;
	uint8_t *id;
	int len;

	TEST_ASSERT(start(&scalar_long_conf, LIB_MINOR, "library") == 0);

	len = read_element_status(1, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	id = &pg.desc[id_offset(1)];
	TEST_CHECK(id[3] == 49);
	TEST_CHECK(memcmp(&id[4], long_identifier, 49) == 0);
	TEST_MSG("identifier: '%.64s'", &id[4]);
	for (int i = 49; i < 64; i++)
		TEST_CHECK_(id[4 + i] == ' ', "pad byte %d is space", i);

	stop();
}

static int inquiry_vpd(uint8_t page, uint8_t *buf, int bufsz)
{
	uint8_t cdb[6] = { 0x12, 0x01, page, 0x00, (uint8_t)bufsz, 0x00 };
	struct scsi_result r;

	memset(buf, 0, bufsz);
	if (harness_send_cdb(&daemon_ctx, cdb, 6, &r, buf, bufsz) != 0)
		return -1;
	if (r.sam_stat != SAM_STAT_GOOD)
		return -1;
	return (int)r.data_len;
}

void test_tape_vpd_matches_dte(void)
{
	uint8_t buf[255];
	int len;

	TEST_ASSERT(start(&scalar_long_conf, TAPE_MINOR, "tape") == 0);

	len = inquiry_vpd(0x80, buf, sizeof(buf));
	TEST_CHECK(len >= 4 + 25);
	TEST_CHECK(buf[3] >= 25);
	TEST_CHECK(memcmp(&buf[4], LONG_SERIAL, 25) == 0);
	TEST_MSG("VPD 80h: '%.*s'", buf[3], &buf[4]);

	len = inquiry_vpd(0x83, buf, sizeof(buf));
	TEST_CHECK(len >= 4 + 4 + 49);
	/* First descriptor: ASCII, T10 vendor ID based */
	TEST_CHECK((buf[4] & 0x0f) == 2);
	TEST_CHECK((buf[5] & 0x0f) == 1);
	TEST_CHECK(buf[7] == 49);
	TEST_CHECK(memcmp(&buf[8], long_identifier, 49) == 0);
	TEST_MSG("VPD 83h T10 id: '%.*s'", buf[7], &buf[8]);
	/* NAA descriptor follows the T10 one */
	TEST_CHECK(buf[8 + 49 + 1] == 0x03);
	TEST_CHECK(get_unaligned_be16(&buf[2]) == 4 + 49 + 12);

	stop();
}

void test_tape_vpd_short_serial_unchanged(void)
{
	uint8_t buf[255];
	int len;

	TEST_ASSERT(start(&scalar_conf, TAPE_MINOR, "tape") == 0);

	len = inquiry_vpd(0x83, buf, sizeof(buf));
	TEST_CHECK(len >= 4 + 50);
	TEST_CHECK(buf[7] == 34);
	TEST_CHECK(memcmp(&buf[8],
			  "IBM     ULTRIUM-TD7     F0027BE0F1", 34) == 0);
	TEST_CHECK(get_unaligned_be16(&buf[2]) == 4 + 34 + 12);

	stop();
}

/* Personalities with a 34 byte identifier still report the first 10
 * characters of a longer serial. On the 3573 the identifier is the
 * drive's VPD 0x83 T10 designator, 22h bytes with a 10 character serial
 * for IBM LTO drives (GA32-0928-06 table 202).
 */
void test_ibm_long_serial_truncated(void)
{
	struct lib_conf c = {
		"IBM", "3573-TL", "IBM", "ULT3580-TD8", LONG_SERIAL
	};
	struct elem_page pg;
	uint8_t *id;
	int len;

	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);

	len = read_element_status(1, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 86);
	id = &pg.desc[id_offset(1)];
	TEST_CHECK(id[3] == 34);
	TEST_CHECK(memcmp(&id[4], "IBM     ULT3580-TD8     QTMSN01234", 34) == 0);
	TEST_MSG("identifier: '%.34s'", &id[4]);

	stop();
}

/* HP MSL reports the serial number alone in a 20 byte identifier */
void test_hp_msl_serial_only_unchanged(void)
{
	struct lib_conf c = {
		"HP", "MSL6000 Series", "HP", "Ultrium 4-SCSI", "723R6KN000"
	};
	struct elem_page pg;
	uint8_t *id;
	int len;

	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);

	/* MSL returns the identifier whether or not DVCID is set */
	len = read_element_status(0, 0);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 36);
	id = &pg.desc[id_offset(0)];
	TEST_CHECK(id[0] == 2 && id[1] == 0 && id[3] == 20);
	TEST_CHECK(memcmp(&id[4], "723R6KN000          ", 20) == 0);
	TEST_MSG("identifier: '%.20s'", &id[4]);

	stop();
}

void test_hp_msl6480_documented_identity(void)
{
	struct lib_conf c = {
		"HP", "MSL6480", "HP", "Ultrium 7-SCSI", "HU1234567A"
	};
	struct elem_page pg;
	uint8_t *id;
	int len;

	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);

	/* The models the HPE reference covers honour DVCID and report the
	 * drive's vendor id, product id and serial number in 34 bytes.
	 */
	len = read_element_status(0, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 50);
	id = &pg.desc[id_offset(0)];
	TEST_CHECK(id[0] == 2 && id[1] == 1 && id[3] == 34);
	TEST_CHECK(memcmp(&id[4],
			  "HP      Ultrium 7-SCSI  HU1234567A", 34) == 0);
	TEST_MSG("identifier: '%.34s'", &id[4]);

	len = read_element_status(0, 0);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 16);

	stop();
}

/* Spectra T-Series reports the serial alone, NUL terminated, in 32 bytes */
void test_spectra_serial_nul_terminated(void)
{
	struct lib_conf c = {
		"SPECTRA", "PYTHON", "IBM", "ULT3580-TD8", LONG_SERIAL
	};
	static const char sn33[] = "SPX0123456789ABCDEFGHIJKLMNOPQRST"; /* 33 */
	struct elem_page pg;
	uint8_t pad[32] = { 0 };
	uint8_t *id;
	int len;

	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);
	len = read_element_status(0, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 16 + 32);
	id = &pg.desc[id_offset(0)];
	TEST_CHECK(id[0] == 2 && id[1] == 0 && id[3] == 32);
	TEST_CHECK(memcmp(&id[4], LONG_SERIAL, 25) == 0);
	TEST_CHECK(memcmp(&id[4 + 25], pad, 32 - 25) == 0);
	TEST_MSG("identifier: '%.32s'", &id[4]);
	stop();

	c.drive_serial = sn33;
	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);
	len = read_element_status(0, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 16 + 32);
	id = &pg.desc[id_offset(0)];
	TEST_CHECK(id[3] == 32);
	TEST_CHECK(memcmp(&id[4], sn33, 31) == 0);
	TEST_CHECK(id[4 + 31] == 0);
	TEST_MSG("identifier: '%.32s'", &id[4]);
	stop();
}

/* IBM LTO drives report a serial shorter than 10 characters right-justified
 * with leading zeroes (GA32-0928-06 tables 200 and 202). The drive's VPD
 * pages and the library's DTE identifier must carry the same value.
 */
void test_ibm_lto_short_serial_zero_padded(void)
{
	struct lib_conf c = {
		"IBM", "3573-TL", "IBM", "ULTRIUM-HH8", "1234567"
	};
	struct elem_page pg;
	uint8_t buf[255];
	uint8_t *id;
	int len;

	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);
	len = read_element_status(0, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	id = &pg.desc[id_offset(0)];
	TEST_CHECK(id[3] == 34);
	TEST_CHECK(memcmp(&id[4], "IBM     ULTRIUM-HH8     0001234567", 34) == 0);
	TEST_MSG("identifier: '%.34s'", &id[4]);
	stop();

	TEST_ASSERT(start(&c, TAPE_MINOR, "tape") == 0);
	len = inquiry_vpd(0x80, buf, sizeof(buf));
	TEST_CHECK(len == 4 + 10);
	TEST_CHECK(buf[3] == 10);
	TEST_CHECK(memcmp(&buf[4], "0001234567", 10) == 0);
	TEST_MSG("VPD 80h: '%.*s'", buf[3], &buf[4]);
	len = inquiry_vpd(0x83, buf, sizeof(buf));
	TEST_CHECK(buf[7] == 34);
	TEST_CHECK(memcmp(&buf[8], "IBM     ULTRIUM-HH8     0001234567", 34) == 0);
	TEST_MSG("VPD 83h T10 id: '%.*s'", buf[7], &buf[8]);
	stop();
}

/* Only IBM LTO drives zero-pad: another vendor's short serial is reported
 * as configured, blank padded.
 */
void test_non_ibm_short_serial_unchanged(void)
{
	struct lib_conf c = {
		"QUANTUM", "Scalar i6000", "HP", "Ultrium 6-SCSI", "ABC12"
	};
	struct elem_page pg;
	uint8_t buf[255];
	uint8_t *id;
	int len;

	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);
	len = read_element_status(0, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	id = &pg.desc[id_offset(0)];
	TEST_CHECK(id[3] == 34);
	TEST_CHECK(memcmp(&id[4], "HP      Ultrium 6-SCSI  ABC12     ", 34) == 0);
	TEST_MSG("identifier: '%.34s'", &id[4]);
	stop();

	TEST_ASSERT(start(&c, TAPE_MINOR, "tape") == 0);
	len = inquiry_vpd(0x80, buf, sizeof(buf));
	TEST_CHECK(memcmp(&buf[4], "ABC12", 5) == 0);
	TEST_MSG("VPD 80h: '%.*s'", buf[3], &buf[4]);
	stop();
}

/* IBM TS3500/3584 (TS3500 SCSI Reference, READ ELEMENT STATUS, table 7):
 * with DVCID=1 the drive descriptor reports only the identity. Except is
 * set with ASC/ASCQ 81/00, Access, Full, ID Valid, the bus address, SValid
 * and the source address are clear, and bytes 12-49 are the drive's VPD
 * 0x83 type 1 designator; the descriptor length is 0x32 for LTO. DVCID=0
 * is unchanged.
 */
void test_ibm3584_dvcid_layout(void)
{
	struct lib_conf c = {
		"IBM", "03584L32", "IBM", "ULT3580-TD8", "F0027BE0F1", 1
	};
	static const uint8_t zero[38] = { 0 };
	struct elem_page pg;
	uint8_t *id;
	int len;

	/* The drive stays empty: loading it needs the tape daemon. The cleared
	 * Full, SValid and source fields of a loaded drive are checked on a
	 * real kernel instead.
	 */
	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);

	len = read_element_status(0, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 0x32);
	TEST_CHECK(get_unaligned_be16(&pg.desc[0]) == 0x0101);
	TEST_CHECK(pg.desc[2] == 0x04);			/* Except only */
	TEST_CHECK(pg.desc[4] == 0x81 && pg.desc[5] == 0x00);
	TEST_CHECK(memcmp(&pg.desc[6], zero, 6) == 0);	/* bytes 6-11 */
	TEST_MSG("bytes 2-11: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
		 pg.desc[2], pg.desc[3], pg.desc[4], pg.desc[5], pg.desc[6],
		 pg.desc[7], pg.desc[8], pg.desc[9], pg.desc[10], pg.desc[11]);
	id = &pg.desc[id_offset(0)];
	TEST_CHECK(id[0] == 2 && id[1] == 1 && id[2] == 0 && id[3] == 0x22);
	TEST_CHECK(memcmp(&id[4],
			  "IBM     ULT3580-TD8     F0027BE0F1", 34) == 0);

	/* Drive 3 has no drive configured: not present, identifier all zero */
	TEST_CHECK(pg.byte_count == 3 * pg.desc_len);
	id = &pg.desc[2 * pg.desc_len];
	TEST_CHECK(get_unaligned_be16(&id[0]) == 0x0103);
	TEST_CHECK(id[2] == 0x04);
	TEST_CHECK(id[4] == 0x82 && id[5] == 0x00);
	TEST_CHECK(memcmp(&id[12], zero, 38) == 0);
	TEST_MSG("drive 3 bytes 2-5: %02x %02x %02x %02x", id[2], id[3], id[4],
		 id[5]);

	/* DVCID=0 still reports the status and the bus address */
	len = read_element_status(0, 0);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 16);
	TEST_CHECK((pg.desc[2] & 0x04) == 0);
	TEST_CHECK(pg.desc[6] == 0x20 && pg.desc[7] == 1);

	stop();
}

/* StorageTek L20/40/80 (doc 95971 tables 4-49/4-50), L180/L700e (doc 95869
 * tables 6-67/6-68) and SL500 (PN 96122 tables 6-76/6-77): with or without
 * DvcID the drive descriptor ends with the serial in a 32 byte area and the
 * four Media/Transport Domain/Type bytes, 88 bytes with PVolTag. The L20 and
 * L700 report the drive's SCSI bus address; the SL500 does not.
 */
void test_stk_dte_layout(void)
{
	static const struct { const char *product; int bus; } models[] = {
		{ "L700", 1 }, { "L80", 1 }, { "SL500", 0 },
	};
	static const uint8_t ff[4] = { 0xff, 0xff, 0xff, 0xff };
	static const uint8_t zero[4] = { 0 };

	for (unsigned i = 0; i < ARRAY_SIZE(models); i++) {
		struct lib_conf c = {
			"STK", models[i].product, "IBM", "ULT3580-TD8", LONG_SERIAL
		};
		struct elem_page pg;
		uint8_t *id;
		int len;

		TEST_CASE_("%s", models[i].product);
		TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);

		/* DvcID=1, PVolTag=1 */
		len = read_element_status(1, 1);
		TEST_ASSERT(find_page(len, 4, &pg) == 0);
		TEST_CHECK(pg.desc_len == 88);
		TEST_CHECK(pg.byte_count == 2 * pg.desc_len);
		TEST_MSG("desc_len %d", pg.desc_len);
		id = &pg.desc[id_offset(1)];
		TEST_CHECK(id[0] == 2 && id[1] == 0 && id[3] == 25);
		TEST_CHECK(memcmp(&id[4], LONG_SERIAL, 25) == 0);
		for (int k = 25; k < 32; k++)
			TEST_CHECK_(id[4 + k] == ' ', "pad byte %d is space", k);
		TEST_CHECK(memcmp(&pg.desc[84], ff, 4) == 0);
		TEST_MSG("identifier: '%.32s'", &id[4]);
		if (models[i].bus) {
			TEST_CHECK(pg.desc[6] == 0x20); /* ID Valid */
			TEST_CHECK(pg.desc[7] == 1);	/* TARGET 01 */
		} else {
			TEST_CHECK(pg.desc[6] == 0 && pg.desc[7] == 0);
		}
		TEST_MSG("byte 6 0x%02x, byte 7 0x%02x", pg.desc[6], pg.desc[7]);

		/* DvcID=1, PVolTag=0: the fields move up */
		len = read_element_status(0, 1);
		TEST_ASSERT(find_page(len, 4, &pg) == 0);
		TEST_CHECK(pg.desc_len == 52);
		TEST_CHECK(memcmp(&pg.desc[id_offset(0) + 4], LONG_SERIAL, 25) == 0);
		TEST_CHECK(memcmp(&pg.desc[48], ff, 4) == 0);

		/* DvcID=0, PVolTag=1: Transport Serial Number at bytes 56-87 */
		len = read_element_status(1, 0);
		TEST_ASSERT(find_page(len, 4, &pg) == 0);
		TEST_CHECK(pg.desc_len == 88);
		TEST_CHECK(memcmp(&pg.desc[48], zero, 4) == 0);
		TEST_CHECK(memcmp(&pg.desc[52], ff, 4) == 0);
		TEST_CHECK(memcmp(&pg.desc[56], LONG_SERIAL, 25) == 0);
		for (int k = 25; k < 32; k++)
			TEST_CHECK_(pg.desc[56 + k] == ' ', "pad byte %d is space", k);
		TEST_MSG("transport serial: '%.32s'", &pg.desc[56]);

		/* DvcID=0, PVolTag=0: the same fields moved up to bytes 12-51 */
		len = read_element_status(0, 0);
		TEST_ASSERT(find_page(len, 4, &pg) == 0);
		TEST_CHECK(pg.desc_len == 52);
		TEST_CHECK(memcmp(&pg.desc[12], zero, 4) == 0);
		TEST_CHECK(memcmp(&pg.desc[16], ff, 4) == 0);
		TEST_CHECK(memcmp(&pg.desc[20], LONG_SERIAL, 25) == 0);
		TEST_CHECK(pg.desc[20 + 25] == ' ' && pg.desc[51] == ' ');

		stop();
	}
}

/* A StorageTek drive serial longer than the 32 byte area is cut to 32
 * characters, and the identifier length says 32.
 */
void test_stk_long_serial_truncated(void)
{
	static const char sn40[] = "STK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0"; /* 40 */
	struct lib_conf c = { "STK", "L700", "IBM", "ULT3580-TD8", sn40 };
	struct elem_page pg;
	uint8_t *id;
	int len;

	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);
	len = read_element_status(1, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 88);
	id = &pg.desc[id_offset(1)];
	TEST_CHECK(id[3] == 32);
	TEST_CHECK(memcmp(&id[4], sn40, 32) == 0);
	TEST_CHECK(pg.desc[84] == 0xff); /* domain/type bytes not overwritten */
	TEST_MSG("identifier: '%.32s'", &id[4]);

	len = read_element_status(1, 0);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(memcmp(&pg.desc[56], sn40, 32) == 0);
	stop();
}

#define OVERLONG_SERIAL "SN0123456789012345678901234567890123456789XYZ" /* 45 */

/* Serials past MAX_DRIVE_SN_LEN are cut to 40 characters on both sides */
void test_overlong_serial_clamped(void)
{
	struct lib_conf c = scalar_long_conf;
	struct elem_page pg;
	uint8_t buf[255];
	uint8_t *id;
	int len;

	c.drive_serial = OVERLONG_SERIAL;

	TEST_ASSERT(start(&c, LIB_MINOR, "library") == 0);
	len = read_element_status(0, 1);
	TEST_ASSERT(find_page(len, 4, &pg) == 0);
	TEST_CHECK(pg.desc_len == 80);
	id = &pg.desc[id_offset(0)];
	TEST_CHECK(id[3] == 64);
	TEST_CHECK(memcmp(&id[4 + 24], OVERLONG_SERIAL, 40) == 0);
	TEST_MSG("identifier: '%.64s'", &id[4]);
	stop();

	TEST_ASSERT(start(&c, TAPE_MINOR, "tape") == 0);
	len = inquiry_vpd(0x80, buf, sizeof(buf));
	TEST_CHECK(len >= 4 + 40);
	TEST_CHECK(buf[3] == 40);
	TEST_CHECK(memcmp(&buf[4], OVERLONG_SERIAL, 40) == 0);
	len = inquiry_vpd(0x83, buf, sizeof(buf));
	TEST_CHECK(len >= 4 + 4 + 64 + 12);
	TEST_CHECK(buf[7] == 64);
	TEST_CHECK(memcmp(&buf[8], &id[4], 64) == 0);
	TEST_MSG("VPD 83h T10 id: '%.*s'", buf[7], &buf[8]);
	stop();
}

/* The Scalar standard INQUIRY carries BarC, the full firmware revision and
 * version descriptors past byte 36; the additional length must cover them.
 */
void test_scalar_inquiry_length(void)
{
	uint8_t cdb[6] = { 0x12, 0x00, 0x00, 0x00, 96, 0x00 };
	struct scsi_result r;
	uint8_t buf[96];

	TEST_ASSERT(start(&scalar_conf, LIB_MINOR, "library") == 0);

	memset(buf, 0, sizeof(buf));
	TEST_ASSERT(harness_send_cdb(&daemon_ctx, cdb, 6, &r, buf,
				     sizeof(buf)) == 0);
	TEST_CHECK(r.sam_stat == SAM_STAT_GOOD);
	TEST_CHECK(r.data_len == 66);
	TEST_CHECK(buf[4] + 5 == 66);
	TEST_MSG("data_len %u, additional length %u", r.data_len, buf[4]);
	TEST_CHECK(memcmp(&buf[8], "QUANTUM ", 8) == 0);
	TEST_CHECK(buf[55] & 0x01);			/* BarC */
	TEST_CHECK(get_unaligned_be16(&buf[64]) == 0x02fe); /* SMC-2 */

	stop();
}

TEST_LIST = {
	{ "scalar_selected_by_vendor",      test_scalar_selected_by_vendor },
	{ "default_personality_consistent", test_default_personality_consistent },
	{ "ibm_layout_unchanged",           test_ibm_layout_unchanged },
	{ "scalar_dvcid_area",              test_scalar_dvcid_area },
	{ "scalar_long_serial",             test_scalar_long_serial },
	{ "tape_vpd_matches_dte",           test_tape_vpd_matches_dte },
	{ "tape_vpd_short_serial_unchanged",test_tape_vpd_short_serial_unchanged },
	{ "ibm_long_serial_truncated",      test_ibm_long_serial_truncated },
	{ "hp_msl_serial_only_unchanged",   test_hp_msl_serial_only_unchanged },
	{ "hp_msl6480_documented_identity", test_hp_msl6480_documented_identity },
	{ "spectra_serial_nul_terminated",  test_spectra_serial_nul_terminated },
	{ "overlong_serial_clamped",        test_overlong_serial_clamped },
	{ "ibm_lto_short_serial_zero_padded", test_ibm_lto_short_serial_zero_padded },
	{ "non_ibm_short_serial_unchanged", test_non_ibm_short_serial_unchanged },
	{ "ibm3584_dvcid_layout",           test_ibm3584_dvcid_layout },
	{ "stk_dte_layout",                 test_stk_dte_layout },
	{ "stk_long_serial_truncated",      test_stk_long_serial_truncated },
	{ "scalar_inquiry_length",          test_scalar_inquiry_length },
	{ NULL, NULL }
};
