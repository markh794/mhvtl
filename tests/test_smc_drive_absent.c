/*
 * Integration tests for a MOVE MEDIUM that involves a drive whose daemon is
 * not running: the library must fail the move instead of waiting forever for
 * a reply, and must not leave the request on the queue for a daemon that
 * starts later.
 *
 * Starts vtllibrary (and vtltape only where a test says so) with
 * MHVTL_BACKEND=userland against a generated device.conf.
 *
 * GC27-2211-00 (IBM TS2900 SCSI Reference) Table 68, p.58:
 *   04/53/00 MEDIA LOAD OR EJECT FAILED
 */
#include <stddef.h>
#include <sys/msg.h>
#include <time.h>

/* A move to an absent drive takes DRIVE_REPLY_GRACE seconds to fail */
#define HARNESS_IO_TIMEOUT_MS 60000

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "be_byteshift.h"

#define LIB_MINOR  10
#define TAPE_MINOR 11

#define BC_LTO8 "E01001L8"

/* IBM 03584 element addresses */
#define DRIVE_ADDR	 0x0101
#define STORAGE_ADDR 0x0400

static struct harness_daemon lib_ctx;
static struct harness_daemon tape_ctx;
static struct test_env test_env;

static int prepare_env(void)
{
	char mktape_path[256];
	char path[1024];
	char cmd[1024];
	FILE *fp;

	memset(&test_env, 0, sizeof(test_env));
	snprintf(test_env.base_dir, sizeof(test_env.base_dir),
		 "/tmp/mhvtl-drvabsent-XXXXXX");
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
		" Vendor identification: IBM\n"
		" Product identification: 03584L32\n"
		" Product revision level: 0100\n"
		" Unit serial number: TESTLIB00\n"
		" NAA: 50:11:22:33:44:55:66:00\n"
		" Home directory: %s\n"
		"\n"
		"Drive: %u CHANNEL: 00 TARGET: 01 LUN: 00\n"
		" Library ID: %u Slot: 1\n"
		" Vendor identification: IBM\n"
		" Product identification: ULT3580-TD8\n"
		" Product revision level: 0100\n"
		" Unit serial number: XYZZY_A\n"
		" NAA: 50:11:22:33:44:55:66:01\n"
		"\n",
		LIB_MINOR, test_env.home_dir, TAPE_MINOR, LIB_MINOR);
	fclose(fp);

	snprintf(path, sizeof(path), "%s/library_contents.%u",
		 test_env.conf_dir, LIB_MINOR);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp, "Drive 1:\nPicker 1:\nMAP 1:\nSlot 1: %s\n", BC_LTO8);
	fclose(fp);

	if (gen_find_binary("mktape", mktape_path, sizeof(mktape_path)) < 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "%s%s -C %s -H %s -l %u -m %s -s 500 -t data -d LTO8 >/dev/null 2>&1",
		 gen_local_lib_prefix(mktape_path), mktape_path,
		 test_env.conf_dir, test_env.home_dir, LIB_MINOR, BC_LTO8);
	return system(cmd);
}

static int start_library(void)
{
	gen_cleanup_env(&test_env);
	if (prepare_env() != 0)
		return -1;
	if (harness_start_env(&lib_ctx, LIB_MINOR, "library", &test_env) != 0)
		return -1;
	harness_clear_ua(&lib_ctx);
	return 0;
}

static int start_drive(void)
{
	if (harness_start_env(&tape_ctx, TAPE_MINOR, "tape", &test_env) != 0)
		return -1;
	harness_clear_ua(&tape_ctx);
	return 0;
}

static void stop(void)
{
	harness_stop(&lib_ctx);
	if (tape_ctx.pid > 0)
		harness_stop(&tape_ctx);
	memset(&tape_ctx, 0, sizeof(tape_ctx));
	gen_cleanup_env(&test_env);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* MOVE MEDIUM; *secs is how long the library took to answer */
static int move_medium(uint16_t src, uint16_t dest, struct scsi_result *r,
		       double *secs)
{
	uint8_t cdb[12] = {
		0xa5, 0x00,
		0x00, 0x01, /* transport element */
		src >> 8, src & 0xff,
		dest >> 8, dest & 0xff,
		0x00, 0x00, 0x00, 0x00
	};
	double t0 = now();
	int rc = harness_cdb(&lib_ctx, cdb, 12, r);

	*secs = now() - t0;
	return rc;
}

/* 1 if the element at 'addr' is full, 0 if empty, -1 on error */
static int element_full(uint16_t addr)
{
	uint8_t cdb[12] = {
		0xb8, 0x00, /* all element types */
		addr >> 8, addr & 0xff,
		0x00, 0x01, /* one element */
		0x00,
		0x00, 0x04, 0x00, /* allocation length 1024 */
		0x00, 0x00
	};
	uint8_t buf[1024];
	struct scsi_result r;

	memset(buf, 0, sizeof(buf));
	if (harness_send_cdb(&lib_ctx, cdb, 12, &r, buf, sizeof(buf)) != 0)
		return -1;
	if (r.sam_stat != SAM_STAT_GOOD || r.data_len < 16 + 3)
		return -1;
	/* Header 8, page header 8, then the descriptor */
	if (get_unaligned_be16(&buf[16]) != addr)
		return -1;
	return buf[16 + 2] & 0x01;
}

static void check_sense(const struct scsi_result *r, uint8_t key,
			uint8_t asc, uint8_t ascq)
{
	TEST_CHECK(r->sam_stat == SAM_STAT_CHECK_CONDITION);
	TEST_CHECK((r->sense[2] & 0x0f) == key);
	TEST_CHECK(r->sense[12] == asc);
	TEST_CHECK(r->sense[13] == ascq);
	TEST_MSG("sam_stat 0x%02x, sense %02x/%02x/%02x, expected %02x/%02x/%02x",
		 r->sam_stat, r->sense[2] & 0x0f, r->sense[12], r->sense[13],
		 key, asc, ascq);
}

/*
 * The texts of the messages waiting for drive 'drv' on the daemons' queue,
 * joined by '|'. They are put back in order, so the drive still gets them.
 */
static void drive_queue(long drv, char *out, size_t len)
{
	struct q_entry e[16];
	int qid = msgget(QKEY, 0);
	int n = 0;

	out[0] = '\0';
	if (qid < 0)
		return;
	while (n < 16 && msgrcv(qid, &e[n], MAXOBN, drv, IPC_NOWAIT | MSG_NOERROR) > 0)
		n++;
	for (int i = 0; i < n; i++) {
		if (i)
			strncat(out, "|", len - strlen(out) - 1);
		strncat(out, e[i].msg.text, len - strlen(out) - 1);
		msgsnd(qid, &e[i], offsetof(struct q_msg, text) + strlen(e[i].msg.text) + 1, 0);
	}
}

static void check_answered_in_grace(double secs)
{
	TEST_CHECK(secs >= DRIVE_REPLY_GRACE - 1 && secs < DRIVE_REPLY_GRACE + 5);
	TEST_MSG("answered after %.1f s, grace %d s", secs, DRIVE_REPLY_GRACE);
}

/* No daemon for the drive: the move fails, the cartridge stays in its slot,
 * and a drive daemon started afterwards finds no stale request and loads
 * the cartridge on the next move.
 */
void test_move_to_absent_drive(void)
{
	struct scsi_result r;
	double secs;

	TEST_ASSERT(start_library() == 0);

	TEST_ASSERT(move_medium(STORAGE_ADDR, DRIVE_ADDR, &r, &secs) == 0);
	check_sense(&r, HARDWARE_ERROR, 0x53, 0x00);
	check_answered_in_grace(secs);
	TEST_CHECK(element_full(STORAGE_ADDR) == 1);
	TEST_CHECK(element_full(DRIVE_ADDR) == 0);

	/* Only the library's startup "Register" waits for the drive */
	char q[256];

	drive_queue(TAPE_MINOR, q, sizeof(q));
	TEST_CHECK(strcmp(q, "Register") == 0);
	TEST_MSG("queue for drive %d: \"%s\"", TAPE_MINOR, q);

	TEST_ASSERT(start_drive() == 0);
	TEST_ASSERT(move_medium(STORAGE_ADDR, DRIVE_ADDR, &r, &secs) == 0);
	TEST_CHECK(r.sam_stat == SAM_STAT_GOOD);
	TEST_MSG("sam_stat 0x%02x, sense %02x/%02x/%02x", r.sam_stat,
		 r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	TEST_CHECK(secs < 5);
	TEST_MSG("move to the running drive took %.1f s", secs);
	TEST_CHECK(element_full(DRIVE_ADDR) == 1);

	/* and moves it out again: an "occupied" answer is not a missing one */
	TEST_ASSERT(move_medium(DRIVE_ADDR, STORAGE_ADDR, &r, &secs) == 0);
	TEST_CHECK(r.sam_stat == SAM_STAT_GOOD);
	TEST_MSG("sam_stat 0x%02x, sense %02x/%02x/%02x", r.sam_stat,
		 r.sense[2] & 0x0f, r.sense[12], r.sense[13]);
	TEST_CHECK(element_full(STORAGE_ADDR) == 1);
	TEST_CHECK(element_full(DRIVE_ADDR) == 0);

	stop();
}

/* The drive daemon stops with a cartridge loaded: moving it out fails and
 * the library still shows it in the drive.
 */
void test_move_from_stopped_drive(void)
{
	struct scsi_result r;
	double secs;

	TEST_ASSERT(start_library() == 0);
	TEST_ASSERT(start_drive() == 0);
	TEST_ASSERT(move_medium(STORAGE_ADDR, DRIVE_ADDR, &r, &secs) == 0);
	TEST_ASSERT(r.sam_stat == SAM_STAT_GOOD);

	harness_stop(&tape_ctx);
	memset(&tape_ctx, 0, sizeof(tape_ctx));

	TEST_ASSERT(move_medium(DRIVE_ADDR, STORAGE_ADDR, &r, &secs) == 0);
	check_sense(&r, HARDWARE_ERROR, 0x53, 0x00);
	check_answered_in_grace(secs);
	TEST_CHECK(element_full(DRIVE_ADDR) == 1);
	TEST_CHECK(element_full(STORAGE_ADDR) == 0);

	stop();
}

TEST_LIST = {
	{ "move_to_absent_drive",    test_move_to_absent_drive },
	{ "move_from_stopped_drive", test_move_from_stopped_drive },
	{ NULL, NULL }
};
