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


static struct harness_daemon daemon_ctx;
static struct test_env test_env;

struct lib_conf {
	const char *vendor;
	const char *product;
	const char *drive_vendor;
	const char *drive_product;
	const char *drive_serial;
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
	fprintf(fp, "Drive 1:\nDrive 2:\nPicker 1:\nMAP 1:\nSlot 1: %s\nSlot 2:\n",
		barcode);
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
				  "IBM     ULT3580-TD8     XYZZY_A   ", 34) == 0);
		/* The second descriptor starts where the length says */
		id = &pg.desc[pg.desc_len + id_offset(voltag)];
		TEST_CHECK(get_unaligned_be16(&pg.desc[pg.desc_len]) ==
			   get_unaligned_be16(&pg.desc[0]) + 1);
		TEST_CHECK(memcmp(&id[4],
				  "IBM     ULT3580-TD8     XYZZY_B   ", 34) == 0);
		TEST_MSG("desc_len %d, byte_count %d, id_len %d",
			 pg.desc_len, pg.byte_count, id[3]);
		/* Header byte count covers exactly what was sent */
		TEST_CHECK(get_unaligned_be24(&res_buf[5]) + 8 == len);
	}

	stop();
}

/* IBM TS3100/TS3200 (3573): code set 2, type 1, 34 byte identifier,
 * GA32-0547-02 p. 3-68.
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
			  "IBM     ULT3580-TD8     XYZZY_A   ", 34) == 0);
	TEST_MSG("identifier: '%.34s'", &id[4]);

	stop();
}

TEST_LIST = {
	{ "scalar_selected_by_vendor",      test_scalar_selected_by_vendor },
	{ "default_personality_consistent", test_default_personality_consistent },
	{ "ibm_layout_unchanged",           test_ibm_layout_unchanged },
	{ NULL, NULL }
};
