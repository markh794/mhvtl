/*
 * Unit tests for the MAM of LTO-8 Type M ("M8") media - markh794/mhvtl#297.
 *
 * M8 is LTO-7 media written in the LTO-8 Type M format. IBM LTO SCSI
 * Reference GA32-0928-07:
 *   table 113, p.142: density 5Dh, "U-832M", "Ultrium 8/32T Type M",
 *                     19,107 bits/mm (as LTO-7), 9,000,000 MB
 *   table 5, p.21:    9,000 GB native, data cartridge only
 *   table 10, p.32:   up to four partitions
 *   table 319, p.352: the same cartridge memory size as L7 and L8 (the
 *                     test compares with L7, not with a fixed value)
 *
 * Checks set_media_params("LTO8M8"), then the MAM mktape writes to a file
 * backed cartridge and the one edit_tape rewrites, read back with
 * load_tape() from libvtlscsi.
 */
#include "acutest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mhvtl_scsi.h"
#include "vtllib.h"
#include "vtlcart.h"
#include "be_byteshift.h"

/* libvtlscsi expects each executable to supply this symbol. */
char mhvtl_driver_name[] = "test_m8_mam";

#define LIB "10"

static char dir[64];

static void check_m8(const struct MAM *m, const char *what)
{
	struct MAM l7;

	init_mam(&l7);
	TEST_ASSERT(set_media_params(&l7, "LTO7") == 0);

	TEST_CHECK_(m->MediaType == Media_LTO8_M8, "%s: MediaType %u", what,
		    m->MediaType);
	TEST_CHECK_(m->MediumDensityCode == 0x5d && m->FormattedDensityCode == 0x5d,
		    "%s: density 0x%02x/0x%02x", what, m->MediumDensityCode,
		    m->FormattedDensityCode);
	TEST_CHECK_(!strncmp(m->media_info.density_name, "U-832M", 6),
		    "%s: density name '%.8s'", what, m->media_info.density_name);
	TEST_CHECK_(!strncmp(m->media_info.description, "Ultrium 8/32T Type M", 20),
		    "%s: description '%.32s'", what, m->media_info.description);
	TEST_CHECK_(!strncmp((const char *)m->AssigningOrganization_1, "LTO-CVE", 7),
		    "%s: assigning organization", what);
	TEST_CHECK_(get_unaligned_be32(&m->media_info.bits_per_mm) == 19107,
		    "%s: bits/mm %u", what,
		    get_unaligned_be32(&m->media_info.bits_per_mm));
	/* LTO-7 media */
	TEST_CHECK_(get_unaligned_be32(&m->MediumLength) ==
			    get_unaligned_be32(&l7.MediumLength) &&
		    get_unaligned_be32(&m->MediumWidth) ==
			    get_unaligned_be32(&l7.MediumWidth),
		    "%s: length and width of LTO-7 media", what);
	TEST_CHECK_(get_unaligned_be64(&m->MAMCapacity) ==
			    get_unaligned_be64(&l7.MAMCapacity),
		    "%s: MAM capacity %llu", what,
		    (unsigned long long)get_unaligned_be64(&m->MAMCapacity));
	TEST_CHECK_(m->max_partitions == 4, "%s: max partitions %u", what,
		    m->max_partitions);
	TEST_CHECK_(m->MediumType == MEDIA_TYPE_DATA, "%s: data medium", what);
}

/* The value is stored in the MAM: it must stay after every existing type */
static void test_media_type_appended(void)
{
	TEST_CHECK(Media_LTO8_M8 == Media_LTO10P + 1);
	TEST_CHECK(Media_LTO10P == 96 && Media_LTO8_M8 == 97);
	TEST_CHECK(Media_LTO8_WORM == 22 && Media_LTO9 == 23);
}

static void test_set_media_params(void)
{
	struct MAM m;

	init_mam(&m);
	m.MediumType = MEDIA_TYPE_DATA;
	TEST_ASSERT(set_media_params(&m, "LTO8M8") == 0);
	check_m8(&m, "set_media_params");
	TEST_CHECK(get_unaligned_be64(&m.max_capacity) == 9000000000000ULL);
	TEST_CHECK(media_native_capacity(Media_LTO8_M8) == 9000000000000ULL);
	TEST_CHECK(media_max_partitions(Media_LTO8_M8) == 4);
	TEST_CHECK(m.num_partitions == 2);
}

static int run(const char *tool, const char *args)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd),
		 "LD_LIBRARY_PATH=../usr MHVTL_CONFIG_PATH=%s MHVTL_HOME_PATH=%s "
		 "../usr/bin/%s %s >/dev/null 2>&1",
		 dir, dir, tool, args);
	return system(cmd);
}

static void load_mam(const char *pcl, const char *what)
{
	uint8_t ss = SAM_STAT_GOOD;

	strncpy(home_directory, dir, HOME_DIR_PATH_SZ);
	TEST_ASSERT_(load_tape(pcl, &ss) == 0, "%s: load_tape(%s)", what, pcl);
	check_m8(&mam, what);
	unload_tape(&ss);
}

static void test_file_backend(void)
{
	char  args[256];
	FILE *fp;

	snprintf(dir, sizeof(dir), "/tmp/mhvtl-m8-XXXXXX");
	TEST_ASSERT(mkdtemp(dir) != NULL);
	snprintf(args, sizeof(args), "%s/device.conf", dir);
	fp = fopen(args, "w");
	TEST_ASSERT(fp != NULL);
	fprintf(fp, "Library: " LIB " CHANNEL: 00 TARGET: 00 LUN: 00\n"
		    " Home directory: %s\n", dir);
	fclose(fp);

	/* mktape, native capacity */
	snprintf(args, sizeof(args),
		 "-C %s -H %s -l " LIB " -m E01001M8 -s 0 -t data -d LTO8M8",
		 dir, dir);
	TEST_ASSERT(run("mktape", args) == 0);
	load_mam("E01001M8", "mktape");
	TEST_CHECK(get_unaligned_be64(&mam.max_capacity) == 9000000000000ULL);

	/* Type M is a data cartridge only */
	snprintf(args, sizeof(args),
		 "-C %s -H %s -l " LIB " -m E01003M8 -s 0 -t WORM -d LTO8M8",
		 dir, dir);
	TEST_CHECK_(run("mktape", args) != 0, "mktape refuses WORM M8");
	snprintf(args, sizeof(args),
		 "-C %s -H %s -l " LIB " -m CLN001M8 -s 0 -t clean -d LTO8M8",
		 dir, dir);
	TEST_CHECK_(run("mktape", args) != 0, "mktape refuses cleaning M8");

	/* edit_tape turns an existing cartridge into M8 */
	snprintf(args, sizeof(args),
		 "-C %s -H %s -l " LIB " -m E01002M8 -s 500 -t data -d LTO7",
		 dir, dir);
	TEST_ASSERT(run("mktape", args) == 0);
	TEST_ASSERT(run("edit_tape", "-l " LIB " -m E01002M8 -d LTO8M8") == 0);
	load_mam("E01002M8", "edit_tape");

	/* edit_tape does not make a cleaning cartridge M8 either */
	snprintf(args, sizeof(args),
		 "-C %s -H %s -l " LIB " -m CLN002L7 -s 0 -t clean -d LTO7",
		 dir, dir);
	TEST_ASSERT(run("mktape", args) == 0);
	TEST_CHECK_(run("edit_tape", "-l " LIB " -m CLN002L7 -d LTO8M8") != 0,
		    "edit_tape refuses to make a cleaning cartridge M8");

	snprintf(args, sizeof(args), "rm -rf %s", dir);
	TEST_CHECK(system(args) == 0);
}

TEST_LIST = {
	{ "media_type_appended", test_media_type_appended },
	{ "set_media_params", test_set_media_params },
	{ "file_backend", test_file_backend },
	{ NULL, NULL }
};
