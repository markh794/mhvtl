/*
 * test_raw_provision — end-to-end integration of mhvtl-raw-provision with a
 * real vtllibrary daemon and a real loopback block device.
 *
 * Driven by tests/test_raw_provision.sh, which formats a loopback device with
 * mhvtl-raw-format and passes <device> <barcode>. This driver:
 *
 *   1. generates an isolated library config (device.conf + library_contents
 *      with one empty MAP slot) via device_conf_gen.h,
 *   2. starts a vtllibrary daemon on that config (MHVTL_QKEY chosen by the
 *      harness, shared with the provisioner through the environment),
 *   3. opens the MAP, writes a bay config binding MAP slot 1 of that library to
 *      the loop device, and runs mhvtl-raw-provision against it,
 *   4. asserts the observable effects:
 *        - home_directory/<barcode> is a symlink to the bay's by-path device,
 *        - the library's MAP slot now reports the probed barcode (via the
 *          "list map" control-message response).
 *
 * Runs as root (the harness formats/opens the loop device). The provisioner is
 * a separate process; it must use the SAME MHVTL_QKEY and config dir as the
 * harness-started library — that lineup is the point of this test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"

static int fails;
#define CHECK(cond, msg) do { \
	if (cond) { printf("  ok   : %s\n", msg); } \
	else { printf("  FAIL : %s\n", msg); fails++; } \
} while (0)

static const struct drive_type *find_drive(const char *name)
{
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++)
		if (strcmp(dt->name, name) == 0)
			return dt;
	return NULL;
}

/* Locate the provisioner binary and the lib dir that holds libvtlscsi.so. */
static int find_provision(char *bin, size_t binsz, char *libdir, size_t libsz)
{
	const char *cands[] = { "usr/bin/", "../usr/bin/", NULL };

	for (int i = 0; cands[i]; i++) {
		snprintf(bin, binsz, "%smhvtl-raw-provision", cands[i]);
		if (access(bin, X_OK) == 0) {
			snprintf(libdir, libsz, "%.*s",
				 (int)(strlen(cands[i]) - 4), cands[i]); /* strip "bin/" */
			return 0;
		}
	}
	return -1;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <block-device> <barcode>\n", argv[0]);
		return 2;
	}
	const char *dev = argv[1];
	const char *barcode = argv[2];

	char prov_bin[256], prov_libdir[256];
	if (find_provision(prov_bin, sizeof(prov_bin),
			   prov_libdir, sizeof(prov_libdir)) < 0) {
		fprintf(stderr, "cannot find mhvtl-raw-provision binary\n");
		return 2;
	}

	const struct drive_type *dt = find_drive("LTO-8");
	if (!dt) { fprintf(stderr, "no LTO-8 drive type\n"); return 2; }

	printf("test_raw_provision on %s (barcode %s)\n", dev, barcode);

	struct test_env env;
	if (gen_test_env(&env, dt) < 0) {
		fprintf(stderr, "gen_test_env failed\n");
		return 2;
	}

	/* Start the library. harness_start_env sets MHVTL_QKEY in our env so the
	 * provisioner we exec below inherits the same per-test queue key. */
	struct harness_daemon lib;
	int started = harness_start_env(&lib, TEST_MINOR_LIBRARY, "library", &env);
	CHECK(started == 0, "vtllibrary started on isolated config");
	if (started != 0) {
		gen_cleanup_env(&env);
		printf("FAILED: %d check(s) failed\n", fails);
		return 1;
	}
	harness_clear_ua(&lib);

	/* Leave the MAP in its default CLOSED state on purpose: mhvtl-raw-provision
	 * must open it itself, load the cartridge, and close it again. That the
	 * barcode still ends up in the MAP below proves the provisioner handled the
	 * closed-MAP precondition (it is not opened for it here). */
	char resp[256] = {0};

	/* Bind MAP slot 1 of this library to the loop device. */
	char bayconf[512];
	snprintf(bayconf, sizeof(bayconf), "%s/mhvtl_raw_bays.conf", env.conf_dir);
	FILE *fp = fopen(bayconf, "w");
	CHECK(fp != NULL, "bay config written");
	if (fp) {
		fprintf(fp, "# test bay map\n%d 1 %s\n", TEST_MINOR_LIBRARY, dev);
		fclose(fp);
	}

	/* Run the provisioner with the SAME config dir + queue key as the library. */
	setenv("MHVTL_CONFIG_PATH", env.conf_dir, 1);
	char cmd[1200];
	snprintf(cmd, sizeof(cmd),
		 "LD_LIBRARY_PATH=%s %s --config %s --verbose",
		 prov_libdir, prov_bin, bayconf);
	printf("--- running: %s\n", cmd);
	int prc = system(cmd);
	CHECK(prc == 0, "mhvtl-raw-provision exited 0");

	/* Effect 1: home_directory/<barcode> -> the STABLE by-path device. */
	char link[512], target[512];
	snprintf(link, sizeof(link), "%s/%s", env.home_dir, barcode);
	ssize_t n = readlink(link, target, sizeof(target) - 1);
	if (n >= 0) target[n] = '\0';
	CHECK(n >= 0, "home_directory/<barcode> symlink created");
	CHECK(n >= 0 && strcmp(target, dev) == 0,
	      "symlink points at the bay by-path device (not resolved /dev/sdX)");

	/* Effect 2: the MAP slot now reports the probed barcode. Poll the library's
	 * "list map" control message until it shows up (load is async over the Q). */
	int in_map = 0;
	for (int i = 0; i < 50 && !in_map; i++) {
		resp[0] = '\0';
		if (harness_vtlcmd(TEST_MINOR_LIBRARY, "list map",
				   resp, sizeof(resp)) == 0 &&
		    strstr(resp, barcode) != NULL)
			in_map = 1;
		else
			usleep(100000);  /* 100ms, 5s total */
	}
	printf("--- list map reports: '%s'\n", resp);
	CHECK(in_map, "MAP slot occupied by the provisioned barcode (READ via 'list map')");

	harness_stop(&lib);
	gen_cleanup_env(&env);

	printf("%s: %d check(s) failed\n", fails ? "FAILED" : "SUCCESS", fails);
	return fails ? 1 : 0;
}
