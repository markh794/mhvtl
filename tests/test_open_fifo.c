/*
 * Unit tests for open_fifo() (usr/vtllib.c), the status FIFO named by
 * "fifo:" in device.conf.
 *
 * The daemons run as root and the path comes from configuration, possibly
 * in a directory others can write to. open_fifo() must never follow a
 * symlink or write into anything but a FIFO owned by the daemon's user.
 */
#include "acutest.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vtllib.h"

/* libvtlscsi expects each executable to supply this symbol. */
char mhvtl_driver_name[] = "test_open_fifo";

static char dir[64];

static void mkdir_tmp(void)
{
	snprintf(dir, sizeof(dir), "/tmp/mhvtl-fifo-XXXXXX");
	TEST_ASSERT(mkdtemp(dir) != NULL);
}

static void cleanup(const char *a, const char *b)
{
	if (a)
		unlink(a);
	if (b)
		unlink(b);
	rmdir(dir);
}

/* A FIFO the daemon creates itself opens and carries data */
void test_creates_and_opens_fifo(void)
{
	char path[128];
	FILE *fp = NULL;
	struct stat st;
	char buf[16] = { 0 };
	int rd;

	mkdir_tmp();
	snprintf(path, sizeof(path), "%s/fifo", dir);

	TEST_CHECK(open_fifo(&fp, path) == 0);
	TEST_ASSERT(fp != NULL);
	TEST_CHECK(lstat(path, &st) == 0 && S_ISFIFO(st.st_mode));

	rd = open(path, O_RDONLY | O_NONBLOCK);
	TEST_ASSERT(rd >= 0);
	fputs("status\n", fp);
	fflush(fp);
	TEST_CHECK(read(rd, buf, sizeof(buf) - 1) == 7);
	TEST_CHECK(strcmp(buf, "status\n") == 0);

	close(rd);
	fclose(fp);
	cleanup(path, NULL);
}

/* An existing FIFO is reused, as before */
void test_reuses_existing_fifo(void)
{
	char path[128];
	FILE *fp = NULL;

	mkdir_tmp();
	snprintf(path, sizeof(path), "%s/fifo", dir);
	TEST_ASSERT(mkfifo(path, 0600) == 0);

	TEST_CHECK(open_fifo(&fp, path) == 0);
	TEST_CHECK(fp != NULL);

	if (fp)
		fclose(fp);
	cleanup(path, NULL);
}

/* A symlink planted at the path is not followed: its target is left as is */
void test_refuses_symlink(void)
{
	char path[128], target[128];
	FILE *fp = NULL;
	struct stat st;
	int fd;

	mkdir_tmp();
	snprintf(path, sizeof(path), "%s/fifo", dir);
	snprintf(target, sizeof(target), "%s/victim", dir);
	fd = open(target, O_WRONLY | O_CREAT, 0644);
	TEST_ASSERT(fd >= 0);
	TEST_ASSERT(write(fd, "precious", 8) == 8);
	close(fd);
	TEST_ASSERT(symlink(target, path) == 0);

	TEST_CHECK(open_fifo(&fp, path) != 0);
	TEST_CHECK(fp == NULL);
	TEST_CHECK(stat(target, &st) == 0 && S_ISREG(st.st_mode));
	TEST_CHECK(st.st_size == 8);
	TEST_MSG("victim size %lld", (long long)st.st_size);

	cleanup(path, target);
}

/* A regular file at the path is not written to */
void test_refuses_regular_file(void)
{
	char path[128];
	FILE *fp = NULL;
	struct stat st;
	int fd;

	mkdir_tmp();
	snprintf(path, sizeof(path), "%s/fifo", dir);
	fd = open(path, O_WRONLY | O_CREAT, 0644);
	TEST_ASSERT(fd >= 0);
	TEST_ASSERT(write(fd, "data", 4) == 4);
	close(fd);

	TEST_CHECK(open_fifo(&fp, path) != 0);
	TEST_CHECK(fp == NULL);
	TEST_CHECK(stat(path, &st) == 0 && st.st_size == 4);

	cleanup(path, NULL);
}

TEST_LIST = {
	{ "creates_and_opens_fifo", test_creates_and_opens_fifo },
	{ "reuses_existing_fifo",   test_reuses_existing_fifo },
	{ "refuses_symlink",        test_refuses_symlink },
	{ "refuses_regular_file",   test_refuses_regular_file },
	{ NULL, NULL }
};
