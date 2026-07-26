/*
 * device_conf_gen.h — Generate isolated test config for any drive type.
 *
 * Each test instance gets its own temp directory containing:
 *   - mhvtl.conf (pointing to itself)
 *   - device.conf (one library + one drive)
 *   - library_contents (tape in slot)
 *   - tape media files
 *   - lock directory
 *   - socket directory
 *
 * No shared state. No root required. Fully parallelizable.
 */

#ifndef _MHVTL_DEVICE_CONF_GEN_H
#define _MHVTL_DEVICE_CONF_GEN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

struct drive_type {
	const char *name;           /* human-readable name */
	const char *vendor;         /* 8-char vendor ID */
	const char *product;        /* 16-char product ID */
	const char *revision;       /* 4-char revision */
	const char *media_barcode;  /* default media barcode */
	const char *density;        /* mktape density: LTO8, T10KB, etc. */
	int         has_compression;
	int         has_encryption;
	int         has_worm;
};

static const struct drive_type ALL_DRIVES[] = {
	/* LTO family */
	{ "LTO-1",  "IBM",     "ULT3580-TD1",     "0100", "E01001L1", "LTO1", 1, 0, 0 },
	{ "LTO-2",  "IBM",     "ULT3580-TD2",     "0210", "E01001L2", "LTO2", 1, 0, 0 },
	{ "LTO-3",  "IBM",     "ULT3580-TD3",     "0310", "E01001L3", "LTO3", 1, 0, 1 },
	{ "LTO-4",  "IBM",     "ULT3580-TD4",     "0460", "E01001L4", "LTO4", 1, 1, 1 },
	{ "LTO-5",  "IBM",     "ULT3580-TD5",     "1050", "E01001L5", "LTO5", 1, 1, 1 },
	{ "LTO-6",  "IBM",     "ULT3580-TD6",     "2160", "E01001L6", "LTO6", 1, 1, 1 },
	{ "LTO-7",  "IBM",     "ULT3580-TD7",     "2720", "E01001L7", "LTO7", 1, 1, 1 },
	{ "LTO-8",  "IBM",     "ULT3580-TD8",     "2160", "E01001L8", "LTO8", 1, 1, 1 },
	{ "LTO-9",  "IBM",     "ULT3580-TD9",     "2160", "E01001L9", "LTO8", 1, 1, 1 },
	/* HP Ultrium */
	{ "HP-LTO4", "HP",     "Ultrium 4-SCSI",  "G24W", "E01001L4", "LTO4", 1, 1, 1 },
	{ "HP-LTO5", "HP",     "Ultrium 5-SCSI",  "I59W", "E01001L5", "LTO5", 1, 1, 1 },
	{ "HP-LTO6", "HP",     "Ultrium 6-SCSI",  "M10W", "E01001L6", "LTO6", 1, 1, 1 },
	/* IBM 3592 */
	{ "3592-J1A", "IBM",   "03592J1A",        "0104", "JA0001JA", "J1A",  1, 0, 1 },
	{ "3592-E05", "IBM",   "03592E05",        "0550", "JB0001JB", "E05",  1, 1, 1 },
	{ "3592-E06", "IBM",   "03592E06",        "0660", "JC0001JC", "E06",  1, 1, 1 },
	{ "3592-E07", "IBM",   "03592E07",        "0770", "JK0001JK", "E07",  1, 1, 1 },
	/* STK T10000 */
	{ "T10000A", "STK",    "T10000A",         "550A", "G03031TA", "T10KA", 1, 1, 0 },
	{ "T10000B", "STK",    "T10000B",         "550V", "G03031TB", "T10KB", 1, 1, 1 },
	{ "T10000C", "STK",    "T10000C",         "550V", "G03031TC", "T10KC", 1, 1, 1 },
	/* STK 9x40 */
	{ "T9840A",  "STK",    "9840A",           "0001", "S09840A0", "9840A", 1, 1, 0 },
	{ "T9840B",  "STK",    "9840B",           "0001", "S09840B0", "9840B", 1, 1, 0 },
	{ "T9840C",  "STK",    "9840C",           "0001", "S09840C0", "9840C", 1, 1, 0 },
	{ "T9840D",  "STK",    "9840D",           "0001", "S09840D0", "9840D", 1, 1, 0 },
	{ "T9940A",  "STK",    "T9940A",          "0001", "S09940A0", "9940A", 1, 1, 0 },
	{ "T9940B",  "STK",    "T9940B",          "0001", "S09940B0", "9940B", 1, 1, 0 },
	/* Quantum DLT/SDLT */
	{ "DLT7000", "QUANTUM", "DLT7000",        "0100", "D07000DL", "DLT4",    1, 0, 0 },
	{ "DLT8000", "QUANTUM", "DLT8000",        "0100", "D08000DL", "DLT4",    1, 0, 0 },
	{ "SDLT320", "QUANTUM", "SDLT320",        "5500", "DS0320SD", "SDLT320", 1, 0, 0 },
	{ "SDLT600", "QUANTUM", "SDLT600",        "5500", "DS0600SD", "SDLT600", 1, 0, 1 },
	/* AIT */
	{ "AIT-1",   "SONY",   "SDX-500C",        "0100", "A0AIT001", "AIT1", 1, 0, 0 },
	{ "AIT-2",   "SONY",   "SDX-700C",        "0100", "A0AIT002", "AIT2", 1, 0, 0 },
	{ "AIT-3",   "SONY",   "SDX-900V",        "0100", "A0AIT003", "AIT3", 1, 0, 0 },
	{ "AIT-4",   "SONY",   "SDX-1100V",       "0100", "A0AIT004", "AIT4", 1, 1, 1 },
	{ NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0 }
};

static int gen_find_binary(const char *name, char *path, size_t path_sz)
{
	const char *candidates[] = {
		"../usr/bin/",
		"usr/bin/",
		"/usr/bin/",
		NULL
	};

	for (int i = 0; candidates[i]; i++) {
		snprintf(path, path_sz, "%s%s", candidates[i], name);
		if (access(path, X_OK) == 0)
			return 0;
	}

	errno = ENOENT;
	return -1;
}

static const char *gen_local_lib_prefix(const char *binary_path)
{
	if (strncmp(binary_path, "../usr/bin/", 11) == 0)
		return "LD_LIBRARY_PATH=../usr ";
	if (strncmp(binary_path, "usr/bin/", 8) == 0)
		return "LD_LIBRARY_PATH=usr ";
	return "";
}

#define TEST_MINOR_TAPE    50
#define TEST_MINOR_LIBRARY 49

/* test_env may already be defined by scsi_harness.h */
#ifndef _MHVTL_TEST_ENV_DEFINED
#define _MHVTL_TEST_ENV_DEFINED
struct test_env {
	char base_dir[256];
	char conf_dir[256];
	char home_dir[256];
	char lock_dir[256];
	char run_dir[256];
};
#endif

static int gen_test_env(struct test_env *env, const struct drive_type *dt)
{
	char path[512];
	char mktape_path[256];
	FILE *fp;

	/* Create unique temp dir */
	snprintf(env->base_dir, sizeof(env->base_dir), "/tmp/mhvtl-test-XXXXXX");
	if (!mkdtemp(env->base_dir))
		return -1;

	snprintf(env->conf_dir, sizeof(env->conf_dir), "%s/conf", env->base_dir);
	snprintf(env->home_dir, sizeof(env->home_dir), "%s/home", env->base_dir);
	snprintf(env->lock_dir, sizeof(env->lock_dir), "%s/lock", env->base_dir);
	snprintf(env->run_dir,  sizeof(env->run_dir),  "%s/run",  env->base_dir);

	mkdir(env->conf_dir, 0755);
	mkdir(env->home_dir, 0755);
	mkdir(env->lock_dir, 0755);
	mkdir(env->run_dir,  0755);

	/* Write mhvtl.conf */
	snprintf(path, sizeof(path), "%s/mhvtl.conf", env->conf_dir);
	fp = fopen(path, "w");
	if (!fp) return -1;
	fprintf(fp,
		"MHVTL_CONFIG_PATH=%s\n"
		"MHVTL_HOME_PATH=%s\n",
		env->conf_dir, env->home_dir);
	fclose(fp);

	/* Write device.conf */
	snprintf(path, sizeof(path), "%s/device.conf", env->conf_dir);
	fp = fopen(path, "w");
	if (!fp) return -1;
	fprintf(fp,
		"VERSION: 5\n"
		"\n"
		"Library: %d CHANNEL: 00 TARGET: 00 LUN: 00\n"
		" Vendor identification: STK\n"
		" Product identification: L700\n"
		" Product revision level: 0108\n"
		" Unit serial number: TESTLIB00\n"
		" NAA: 50:11:22:33:44:55:66:00\n"
		" Home directory: %s\n"
		"\n"
		"Drive: %d CHANNEL: 00 TARGET: 01 LUN: 00\n"
		" Library ID: %d Slot: 1\n"
		" Vendor identification: %-8s\n"
		" Product identification: %-16s\n"
		" Product revision level: %s\n"
		" Unit serial number: XYZZY_%02d\n"
		" NAA: 50:11:22:33:44:55:66:01\n"
		"\n",
		TEST_MINOR_LIBRARY,
		env->home_dir,
		TEST_MINOR_TAPE, TEST_MINOR_LIBRARY,
		dt->vendor, dt->product, dt->revision,
		TEST_MINOR_TAPE);
	fclose(fp);

	/* Write library_contents */
	snprintf(path, sizeof(path), "%s/library_contents.%d",
		 env->conf_dir, TEST_MINOR_LIBRARY);
	fp = fopen(path, "w");
	if (!fp) return -1;
	fprintf(fp,
		"Drive 1: %s\n"
		"Slot 1: %s\n"
		"Slot 2:\n"
		"MAP 1:\n",
		dt->media_barcode, dt->media_barcode);
	fclose(fp);

	/* Create tape media — try local build first, then installed */
	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;

	snprintf(path, sizeof(path),
		 "%s%s -C %s -H %s -l %d -m %s -s 500 -t data -d %s >/dev/null 2>&1",
		 gen_local_lib_prefix(mktape_path),
		 mktape_path,
		 env->conf_dir, env->home_dir,
		 TEST_MINOR_LIBRARY, dt->media_barcode, dt->density);
	if (system(path) != 0)
		return -1;

	return 0;
}

static void gen_cleanup_env(struct test_env *env)
{
	if (env->base_dir[0]) {
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "rm -rf %s", env->base_dir);
		system(cmd);
		env->base_dir[0] = '\0';
	}
}

#endif /* _MHVTL_DEVICE_CONF_GEN_H */
