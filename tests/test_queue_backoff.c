/*
 * Integration tests for queue command latency after idle -
 * markh794/mhvtl#268.
 *
 * vtllibrary and vtltape sleep between polls and lengthen the sleep by
 * "Backoff" each idle cycle, up to 1 s. A queue message must bring the
 * interval back to the minimum, as a SCSI command does, or each command in
 * a burst waits up to a second for its turn. "exit" must not: the loops
 * exit only once the interval has backed off past 0xf000 us.
 *
 * Starts the daemons with MHVTL_BACKEND=userland against a generated
 * device.conf, as test_smc_dvcid.c does.
 */
#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"

#include <time.h>

#define LIB_MINOR  10
#define TAPE_MINOR 11
#define BURST	   16

static struct harness_daemon daemon_ctx;
static struct test_env test_env;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int prepare_env(int backoff)
{
	char path[1024];
	FILE *fp;

	memset(&test_env, 0, sizeof(test_env));
	snprintf(test_env.base_dir, sizeof(test_env.base_dir),
		 "/tmp/mhvtl-backoff-XXXXXX");
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
		" Vendor identification: STK\n"
		" Product identification: L700\n"
		" Product revision level: 0100\n"
		" Unit serial number: TESTLIB00\n"
		" NAA: 50:11:22:33:44:55:66:00\n"
		" Home directory: %s\n"
		" Backoff: %d\n"
		"\n"
		"Drive: %u CHANNEL: 00 TARGET: 01 LUN: 00\n"
		" Library ID: %u Slot: 1\n"
		" Vendor identification: IBM\n"
		" Product identification: ULT3580-TD8\n"
		" Product revision level: 0100\n"
		" Unit serial number: XYZZY_A\n"
		" NAA: 50:11:22:33:44:55:66:01\n"
		" Backoff: %d\n"
		"\n",
		LIB_MINOR, test_env.home_dir, backoff,
		TAPE_MINOR, LIB_MINOR, backoff);
	fclose(fp);

	snprintf(path, sizeof(path), "%s/library_contents.%u",
		 test_env.conf_dir, LIB_MINOR);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp, "Drive 1:\nPicker 1:\nMAP 1:\nSlot 1:\nSlot 2:\n");
	fclose(fp);
	return 0;
}

static int start(unsigned minor, const char *type, int backoff)
{
	gen_cleanup_env(&test_env);
	if (prepare_env(backoff) != 0)
		return -1;
	return harness_start_env(&daemon_ctx, minor, type, &test_env);
}

static void stop(void)
{
	harness_stop(&daemon_ctx);
	gen_cleanup_env(&test_env);
}

/*
 * Queue BURST copies of cmd to minor at once, then wait for every reply.
 * Returns the seconds until the last reply, or -1 if any is missing after
 * 'limit' seconds.
 */
static double burst(unsigned minor, const char *cmd, double limit)
{
	long reply_type = VTLCMD_Q + getpid();
	struct q_entry s, r;
	double t0, deadline;
	int qid, got = 0;

	qid = msgget(QKEY, 0);
	if (qid < 0)
		return -1;

	memset(&s, 0, sizeof(s));
	s.rcv_id = minor;
	s.msg.snd_id = reply_type;
	strncpy(s.msg.text, cmd, MAXTEXTLEN);

	t0 = now();
	for (int i = 0; i < BURST; i++)
		if (msgsnd(qid, &s, strlen(cmd) + sizeof(long) + 1, 0) < 0)
			return -1;

	deadline = t0 + limit;
	while (got < BURST && now() < deadline) {
		if (msgrcv(qid, &r, MAXOBN, reply_type, IPC_NOWAIT) >= 0)
			got++;
		else
			usleep(1000);
	}
	return got == BURST ? now() - t0 : -1;
}

/*
 * With Backoff 9999, 10 s of idle takes the interval to about 0.45 s.
 * Before the fix the burst took about BURST times that; now each message
 * resets the interval, so the whole burst is quick.
 */
static void check_burst(unsigned minor, const char *type, const char *cmd)
{
	double t;

	TEST_ASSERT(start(minor, type, 9999) == 0);
	sleep(10);
	t = burst(minor, cmd, 15.0);
	TEST_CHECK(t >= 0);
	TEST_CHECK(t < 2.0);
	TEST_MSG("%d \"%s\" replies took %.3f s", BURST, cmd, t);
	stop();
}

static void test_library_burst_after_idle(void)
{
	check_burst(LIB_MINOR, "library", "list map");
}

static void test_tape_burst_after_idle(void)
{
	check_burst(TAPE_MINOR, "tape", msg_mount_state);
}

/*
 * After 6 s idle at the default Backoff the interval is past the 0xf000
 * exit gate, so "exit" is acted on at once. Had "exit" reset the interval,
 * the daemon would back off for about 4.7 s first.
 */
static void check_exit(unsigned minor, const char *type)
{
	double t0;
	int status;
	pid_t pid = 0;

	TEST_ASSERT(start(minor, type, 400) == 0);
	sleep(6);
	t0 = now();
	TEST_ASSERT(harness_vtlcmd_no_response(minor, "exit") == 0);
	while (now() - t0 < 10.0) {
		pid = waitpid(daemon_ctx.pid, &status, WNOHANG);
		if (pid == daemon_ctx.pid)
			break;
		usleep(10000);
	}
	TEST_CHECK(pid == daemon_ctx.pid);
	TEST_CHECK(now() - t0 < 2.0);
	TEST_MSG("exit took %.3f s", now() - t0);
	if (pid == daemon_ctx.pid)
		daemon_ctx.pid = -1;
	stop();
}

static void test_library_exit_after_idle(void)
{
	check_exit(LIB_MINOR, "library");
}

static void test_tape_exit_after_idle(void)
{
	check_exit(TAPE_MINOR, "tape");
}

TEST_LIST = {
	{ "library_burst_after_idle", test_library_burst_after_idle },
	{ "tape_burst_after_idle",    test_tape_burst_after_idle },
	{ "library_exit_after_idle",  test_library_exit_after_idle },
	{ "tape_exit_after_idle",     test_tape_exit_after_idle },
	{ NULL, NULL }
};
