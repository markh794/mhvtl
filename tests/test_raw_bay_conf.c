/*
 * test_raw_bay_conf — unit tests for the raw-disk bay map parser
 * (usr/raw_bay_conf.c). Writes temp config files and checks parsing, comment/
 * blank handling, the optional by-id field, and malformed-line rejection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "raw_bay_conf.h"

/* Write `body` to a fresh temp file; returns a malloc'd path (caller frees). */
static char *write_tmp(const char *body)
{
	char *path = strdup("/tmp/mhvtl-baycfg-XXXXXX");
	int fd = mkstemp(path);
	if (fd < 0) {
		free(path);
		return NULL;
	}
	if (body && *body)
		(void)!write(fd, body, strlen(body));
	close(fd);
	return path;
}

void test_valid_multi_bay(void)
{
	char *p = write_tmp(
		"10 1 /dev/disk/by-path/pci-0000:01:00.0-sas-phy0-lun-0\n"
		"10 2 /dev/disk/by-path/pci-0000:01:00.0-sas-phy1-lun-0 by-id=wwn-0x5000c500aabbccdd\n");
	struct raw_bay *bays = NULL;
	size_t n = 0;
	char err[256] = "";

	TEST_ASSERT(p != NULL);
	TEST_CHECK_(raw_bay_conf_parse(p, &bays, &n, err, sizeof(err)) == 0,
		    "parse ok: %s", err);
	TEST_CHECK_(n == 2, "got %zu bays (expected 2)", n);
	if (n == 2) {
		TEST_CHECK(bays[0].library_id == 10 && bays[0].map_slot == 1);
		TEST_CHECK(strcmp(bays[0].by_path,
			"/dev/disk/by-path/pci-0000:01:00.0-sas-phy0-lun-0") == 0);
		TEST_CHECK_(bays[0].by_id[0] == '\0', "bay0 by_id empty");
		TEST_CHECK(bays[1].library_id == 10 && bays[1].map_slot == 2);
		TEST_CHECK_(strcmp(bays[1].by_id, "wwn-0x5000c500aabbccdd") == 0,
			    "bay1 by_id='%s'", bays[1].by_id);
	}
	raw_bay_conf_free(bays);
	unlink(p);
	free(p);
}

void test_comments_and_blanks(void)
{
	char *p = write_tmp(
		"# a comment\n"
		"\n"
		"   # indented comment\n"
		"10 1 /dev/loop7\n"
		"\n"
		"30 4 /dev/loop8\n");
	struct raw_bay *bays = NULL;
	size_t n = 0;
	char err[256] = "";

	TEST_ASSERT(p != NULL);
	TEST_CHECK_(raw_bay_conf_parse(p, &bays, &n, err, sizeof(err)) == 0,
		    "parse ok: %s", err);
	TEST_CHECK_(n == 2, "got %zu bays (expected 2)", n);
	if (n == 2) {
		TEST_CHECK(bays[0].map_slot == 1 && bays[1].map_slot == 4);
		TEST_CHECK(bays[1].library_id == 30);
	}
	raw_bay_conf_free(bays);
	unlink(p);
	free(p);
}

void test_empty_file(void)
{
	char *p = write_tmp("# only comments\n\n");
	struct raw_bay *bays = (void *)0x1;
	size_t n = 99;
	char err[256] = "";

	TEST_ASSERT(p != NULL);
	TEST_CHECK(raw_bay_conf_parse(p, &bays, &n, err, sizeof(err)) == 0);
	TEST_CHECK_(n == 0, "got %zu (expected 0)", n);
	TEST_CHECK(bays == NULL);
	raw_bay_conf_free(bays);
	unlink(p);
	free(p);
}

void test_malformed_missing_field(void)
{
	char *p = write_tmp("10 1\n");   /* no by-path */
	struct raw_bay *bays = NULL;
	size_t n = 0;
	char err[256] = "";

	TEST_ASSERT(p != NULL);
	TEST_CHECK(raw_bay_conf_parse(p, &bays, &n, err, sizeof(err)) == -1);
	TEST_CHECK_(strstr(err, "line 1") != NULL, "err names line 1: '%s'", err);
	TEST_CHECK(bays == NULL && n == 0);
	unlink(p);
	free(p);
}

void test_malformed_bad_int(void)
{
	char *p = write_tmp(
		"10 1 /dev/loop0\n"
		"abc 2 /dev/loop1\n");   /* library_id not an int, on line 2 */
	struct raw_bay *bays = NULL;
	size_t n = 0;
	char err[256] = "";

	TEST_ASSERT(p != NULL);
	TEST_CHECK(raw_bay_conf_parse(p, &bays, &n, err, sizeof(err)) == -1);
	TEST_CHECK_(strstr(err, "line 2") != NULL, "err names line 2: '%s'", err);
	unlink(p);
	free(p);
}

void test_malformed_extra_field(void)
{
	char *p = write_tmp("10 1 /dev/loop0 by-id=x junk\n");
	struct raw_bay *bays = NULL;
	size_t n = 0;
	char err[256] = "";

	TEST_ASSERT(p != NULL);
	TEST_CHECK(raw_bay_conf_parse(p, &bays, &n, err, sizeof(err)) == -1);
	unlink(p);
	free(p);
}

void test_malformed_unknown_4th(void)
{
	char *p = write_tmp("10 1 /dev/loop0 notbyid=x\n");
	struct raw_bay *bays = NULL;
	size_t n = 0;
	char err[256] = "";

	TEST_ASSERT(p != NULL);
	TEST_CHECK(raw_bay_conf_parse(p, &bays, &n, err, sizeof(err)) == -1);
	unlink(p);
	free(p);
}

TEST_LIST = {
	{ "valid_multi_bay",        test_valid_multi_bay },
	{ "comments_and_blanks",    test_comments_and_blanks },
	{ "empty_file",             test_empty_file },
	{ "malformed_missing_field",test_malformed_missing_field },
	{ "malformed_bad_int",      test_malformed_bad_int },
	{ "malformed_extra_field",  test_malformed_extra_field },
	{ "malformed_unknown_4th",  test_malformed_unknown_4th },
	{ NULL, NULL }
};
