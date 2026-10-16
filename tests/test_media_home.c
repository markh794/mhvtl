/*
 * Unit tests for find_media_home_directory() (usr/vtllib.c): a library's home
 * is its own "Home directory:" in device.conf, else MHVTL_HOME_PATH. A library
 * without one must not take the home of a library listed after it.
 */
#include "acutest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vtllib.h"

/* libvtlscsi expects each executable to supply this symbol. */
char mhvtl_driver_name[] = "test_media_home";

static char dir[64];

static void write_conf(const char *text)
{
	char  path[128];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/device.conf", dir);
	fp = fopen(path, "w");
	TEST_ASSERT(fp != NULL);
	fputs(text, fp);
	fclose(fp);
}

static void home_is(long lib, const char *want)
{
	find_media_home_directory(dir, lib);
	TEST_CHECK(strcmp(home_directory, want) == 0);
	TEST_MSG("library %ld: got '%s', want '%s'", lib, home_directory, want);
}

static void test_home_not_taken_from_later_library(void)
{
	snprintf(dir, sizeof(dir), "/tmp/mhvtl-home-XXXXXX");
	TEST_ASSERT(mkdtemp(dir) != NULL);
	setenv("MHVTL_HOME_PATH", "/var/lib/fallback", 1);

	write_conf("Library: 10 CHANNEL: 00 TARGET: 00 LUN: 00\n"
			   " Vendor identification: STK\n"
			   " Unit serial number: LIB10\n"
			   "\n"
			   "Library: 20 CHANNEL: 00 TARGET: 08 LUN: 00\n"
			   " Vendor identification: STK\n"
			   " Unit serial number: LIB20\n"
			   " Home directory: /srv/vtl20\n");
	home_is(10, "/var/lib/fallback");
	home_is(20, "/srv/vtl20");

	/* A library with no home that is followed by drives, then a library with one */
	write_conf("Library: 10 CHANNEL: 00 TARGET: 00 LUN: 00\n"
			   " Unit serial number: LIB10\n"
			   "\n"
			   "Drive: 11 CHANNEL: 00 TARGET: 01 LUN: 00\n"
			   " Library ID: 10 Slot: 01\n"
			   " Home directory: /srv/drive-not-library\n"
			   "\n"
			   "Library: 20 CHANNEL: 00 TARGET: 08 LUN: 00\n"
			   " Home directory: /srv/vtl20\n");
	home_is(10, "/var/lib/fallback");
	home_is(20, "/srv/vtl20");

	/* Own home first, later library's home second */
	write_conf("Library: 10 CHANNEL: 00 TARGET: 00 LUN: 00\n"
			   " Home directory: /srv/vtl10\n"
			   "\n"
			   "Library: 20 CHANNEL: 00 TARGET: 08 LUN: 00\n"
			   " Home directory: /srv/vtl20\n");
	home_is(10, "/srv/vtl10");
	home_is(20, "/srv/vtl20");

	{
		char path[128];
		snprintf(path, sizeof(path), "%s/device.conf", dir);
		unlink(path);
		rmdir(dir);
	}
}

TEST_LIST = {
	{"home_not_taken_from_later_library", test_home_not_taken_from_later_library},
	{NULL, NULL}};
