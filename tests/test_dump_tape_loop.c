/*
 * dump_tape must terminate and print each block of a cartridge exactly once,
 * with and without -D, and report filemarks. Regression test for the loop in
 * dump_tape_metadata() that re-printed block 0 forever once 101f3b2 made the
 * advance conditional on -D or a filemark.
 *
 * Builds a file-backed cartridge (3 data blocks, a filemark, 2 data blocks)
 * with mktape + the libvtlscsi tape layer, then runs dump_tape (found by
 * gen_find_binary) against it under a timeout and a 1 MiB output cap, so a
 * regression fails instead of hanging or filling $TMPDIR.
 */
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "acutest.h"
#include "mhvtl_scsi.h"
#include "vtlcart.h"
#include "vtllib.h"
/* the helper header also holds env generators this test does not use */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "helpers/device_conf_gen.h"
#pragma GCC diagnostic pop

char mhvtl_driver_name[] = "test_dump_tape_loop";

#define BARCODE		"DUMP01L5"
#define BLK_SZ		512
#define TIMEOUT_SEC 10

static char base[256];
static char conf_dir[300];
static char home[300];
static char out_path[300];
static char err_path[300];

static void cleanup(void) {
	char cmd[600];

	if (!base[0])
		return;
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
	if (system(cmd) != 0)
		fprintf(stderr, "cleanup of %s failed\n", base);
	base[0] = '\0';
}

static int make_cartridge(void) {
	const char *tmp = getenv("TMPDIR");
	char		cmd[1024];
	char		path[320];
	char		mktape_path[256];
	uint8_t		buf[BLK_SZ];
	uint8_t		ss = SAM_STAT_GOOD;
	FILE	   *fp;

	snprintf(base, sizeof(base), "%s/mhvtl-dumploop-XXXXXX", (tmp && tmp[0]) ? tmp : "/tmp");
	if (!mkdtemp(base))
		return -1;
	snprintf(conf_dir, sizeof(conf_dir), "%s/conf", base);
	snprintf(home, sizeof(home), "%s/home", base);
	snprintf(out_path, sizeof(out_path), "%s/out.txt", base);
	snprintf(err_path, sizeof(err_path), "%s/err.txt", base);
	if (mkdir(conf_dir, 0755) || mkdir(home, 0755))
		return -1;

	snprintf(path, sizeof(path), "%s/mhvtl.conf", conf_dir);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp, "MHVTL_CONFIG_PATH=%s\nMHVTL_HOME_PATH=%s\n", conf_dir, home);
	fclose(fp);

	snprintf(path, sizeof(path), "%s/device.conf", conf_dir);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp,
			"VERSION: 5\n\n"
			"Library: 1 CHANNEL: 00 TARGET: 00 LUN: 00\n"
			" Vendor identification: STK\n"
			" Product identification: L700\n"
			" Product revision level: 0108\n"
			" Unit serial number: TESTLIB00\n"
			" NAA: 50:11:22:33:44:55:66:00\n"
			" Home directory: %s\n\n",
			home);
	fclose(fp);

	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
			 "%s%s -C '%s' -H '%s' -l 1 -m %s -s 4 -t data -d LTO5 >/dev/null 2>&1",
			 gen_local_lib_prefix(mktape_path), mktape_path, conf_dir, home, BARCODE);
	if (system(cmd) != 0)
		return -1;

	strncpy(home_directory, home, HOME_DIR_PATH_SZ);
	home_directory[HOME_DIR_PATH_SZ] = '\0';
	if (load_tape(BARCODE, &ss) || ss != SAM_STAT_GOOD)
		return -1;
	rewind_tape(&ss);
	for (int i = 0; i < 5; i++) {
		if (i == 3 && write_filemarks(1, &ss))
			return -1;
		memset(buf, 'A' + i, sizeof(buf));
		if (write_tape_block(buf, sizeof(buf), 0, NULL, 0, 0, 0, &ss))
			return -1;
	}
	unload_tape(&ss);
	return 0;
}

/* Run dump_tape as prog (dump_tape or preload_tape: the personality comes from
 * argv[0], and preload_tape is only a link made at install time) with the NULL-terminated args, stdout
 * and stderr to files. Returns 0 on clean exit, -1 on timeout, -2 on non-zero
 * exit, -3 if killed by a signal (a crash, or the file size limit that stops
 * a runaway dump). */
static int run_dump(const char *prog, char *const *args) {
	char  bin[256];
	pid_t pid;
	int	  status, waited;

	if (gen_find_binary("dump_tape", bin, sizeof(bin)) < 0)
		return -2;
	pid = fork();
	if (pid < 0)
		return -2;
	if (pid == 0) {
		char		 *argv[12] = {(char *)prog};
		struct rlimit lim	  = {1 << 20, 1 << 20};

		for (int i = 0; i < 10 && args[i]; i++)
			argv[i + 1] = args[i];
		setrlimit(RLIMIT_FSIZE, &lim); /* a looping dump must not fill $TMPDIR */
		signal(SIGXFSZ, SIG_DFL);
		setenv("MHVTL_CONFIG_PATH", conf_dir, 1);
		setenv("LD_LIBRARY_PATH", "../usr", 1);
		if (!freopen(out_path, "w", stdout) || !freopen(err_path, "w", stderr))
			_exit(126);
		execv(bin, argv);
		_exit(127);
	}
	for (waited = 0; waited < TIMEOUT_SEC * 20; waited++) {
		if (waitpid(pid, &status, WNOHANG) == pid) {
			if (WIFSIGNALED(status))
				return -3;
			return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -2;
		}
		usleep(50000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return -1;
}

static int count_in(const char *path, const char *needle) {
	char  *line = NULL;
	size_t len	= 0;
	int	   n	= 0;
	FILE  *fp	= fopen(path, "r");

	if (!fp)
		return -1;
	while (getline(&line, &len, fp) > 0)
		if (strstr(line, needle))
			n++;
	free(line);
	fclose(fp);
	return n;
}

static int count_lines(const char *needle) {
	return count_in(out_path, needle);
}

static int err_has(const char *needle) {
	return count_in(err_path, needle);
}

static int setup(void) {
	int ok = make_cartridge() == 0;

	if (!ok)
		cleanup();
	return ok;
}

static void check_dump(const char *flag) {
	char *args[] = {"-m", BARCODE, (char *)flag, NULL};
	int	  rc	 = run_dump("dump_tape", args);

	TEST_CHECK_(rc != -1, "dump_tape %s timed out (loops)", flag ? flag : "");
	TEST_CHECK_(rc == 0, "dump_tape %s exit status", flag ? flag : "");
	if (rc != 0)
		return;
	TEST_CHECK_(count_lines("Hdr:non-compressed") == 5, "5 data block headers, got %d", count_lines("Hdr:non-compressed"));
	for (int i = 0; i < 6; i++) {
		char want[32];

		if (i == 3)
			continue;
		snprintf(want, sizeof(want), "Blk No.: %7d,", i);
		TEST_CHECK_(count_lines(want) == 1, "block %d printed once, got %d", i, count_lines(want));
	}
	TEST_CHECK_(count_lines("Filemark at block : 3 , skipping") == 1, "filemark reported once");
	TEST_CHECK_(count_lines("Hdr:End of Data") == 1, "EOD reached once");
}

static void test_dump_terminates(void) {
	TEST_ASSERT_(setup(), "create test cartridge");
	check_dump("-v"); /* metadata only: must still advance */
	check_dump("-D"); /* with data reads */
	cleanup();
}

/* A header whose block number goes backwards must end the dump, not loop */
static void test_backwards_block_number(void) {
	char	 path[320];
	uint32_t bad = 1;
	FILE	*fp;
	char	*args[] = {"-m", BARCODE, "-v", NULL};

	TEST_ASSERT_(setup(), "create test cartridge");
	/* indx.0 is one 512-byte raw_header per block; blk_number is at +16
	 * (loff_t data_offset, then blk_type, blk_flags, blk_number). */
	snprintf(path, sizeof(path), "%s/%s/indx.0", home, BARCODE);
	fp = fopen(path, "r+");
	TEST_ASSERT_(fp != NULL, "open %s", path);
	TEST_ASSERT_(fseek(fp, 4 * 512 + 16, SEEK_SET) == 0 && fwrite(&bad, sizeof(bad), 1, fp) == 1, "patch index");
	fclose(fp);

	TEST_CHECK_(run_dump("dump_tape", args) == 0, "dump_tape terminates");
	TEST_CHECK_(err_has("Unable to advance past block 3") == 1, "backwards header reported");
	cleanup();
}

TEST_LIST = {
	{"dump_tape prints each block once", test_dump_terminates},
	{"dump_tape backwards block number", test_backwards_block_number},
	{NULL, NULL},
};
