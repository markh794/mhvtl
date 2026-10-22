/*
 * dump_tape / preload_tape option parsing: -l selects the library, and an
 * option that takes a value must have one. Regression test for -l parsing
 * atoi(argv[1]) (the "-l" itself) and for value options reading argv[argc]
 * (NULL) when given last, which crashed preload_tape -b.
 *
 * Builds an empty file-backed cartridge with mktape and runs dump_tape (found
 * by gen_find_binary) under a timeout, stdout and stderr to files.
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
/* the helper header also holds env generators this test does not use */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "helpers/device_conf_gen.h"
#pragma GCC diagnostic pop

char mhvtl_driver_name[] = "test_dump_tape_options";

#define BARCODE		"OPTS01L5"
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
	FILE	   *fp;

	snprintf(base, sizeof(base), "%s/mhvtl-dumpopts-XXXXXX", (tmp && tmp[0]) ? tmp : "/tmp");
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

	return 0;
}

/* Run dump_tape as prog (dump_tape or preload_tape: the personality comes from
 * argv[0], and preload_tape is only a link made at install time) with the NULL-terminated args, stdout
 * and stderr to files. Returns 0 on clean exit, -1 on timeout, -2 on non-zero
 * exit, -3 if killed by a signal (a crash, or the file size limit that stops
 * a runaway process). */
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

/* -l selects the library whose home holds the media; -m needs a value */
static void test_option_parsing(void) {
	char *lib1[]	 = {"-l", "1", "-m", BARCODE, NULL};
	char *lib2[]	 = {"-l", "2", "-m", BARCODE, NULL};
	char *no_val[]	 = {"-m", NULL};
	char *dash_val[] = {"-m", "-x", NULL};
	char *no_blk[]	 = {"-m", "X", "-c", "NONE", "-F", "f", "-b", NULL};

	TEST_ASSERT_(setup(), "create test cartridge");
	TEST_CHECK_(run_dump("dump_tape", lib1) == 0, "-l 1 finds the media");
	TEST_CHECK_(run_dump("dump_tape", lib2) == -2, "-l 2 (no such library) must not find the media");

	TEST_CHECK_(run_dump("dump_tape", no_val) == -2 && count_lines("More args needed for -m") == 1,
				"-m without a value is a usage error");
	TEST_CHECK_(run_dump("dump_tape", dash_val) == -2 && err_has("PCL -x cannot be opened") == 1,
				"a value starting with '-' is taken as the value");
	TEST_CHECK_(run_dump("preload_tape", no_blk) == -2 && count_lines("-b requires additional block size") == 1,
				"preload_tape -b without a value is a usage error, not a crash");
	cleanup();
}

TEST_LIST = {
	{"dump_tape option parsing", test_option_parsing},
	{NULL, NULL},
};
