/*
 * test_tape_io.c — Write/read/verify data for every supported drive type.
 *
 * All daemons start in parallel at the beginning, then tests run
 * against already-running daemons. Each daemon has its own isolated
 * temp directory — no conflicts.
 *
 * Usage: ./test_tape_io
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"

/* One slot per drive type: env + daemon + socket */
struct drive_slot {
	const struct drive_type *dt;
	struct test_env          env;
	struct harness_daemon    daemon;
	int                      ready;  /* daemon started + tape loaded */
};

static struct drive_slot slots[64];
static int num_slots;

/* ---- Setup: start all daemons in parallel ---- */

static void setup_all(void)
{
	num_slots = 0;

	/* Phase 1: generate configs + create media + fork daemons (no waiting) */
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++) {
		struct drive_slot *s = &slots[num_slots];
		s->dt = dt;
		s->ready = 0;

		if (gen_test_env(&s->env, dt) < 0)
			continue;

		/* Set unique QKEY before fork so parent and child share it */
		char qkey[32];
		snprintf(qkey, sizeof(qkey), "0x%x",
			 (unsigned)(MHVTL_QKEY_BASE + getpid() * 100 + num_slots));
		setenv("MHVTL_QKEY", qkey, 1);

		/* Fork daemon (non-blocking — returns immediately) */
		memset(&s->daemon, 0, sizeof(s->daemon));
		s->daemon.minor = TEST_MINOR_TAPE;
		s->daemon.pid = -1;
		s->daemon.sock_fd = -1;

		snprintf(s->daemon.sock_path, sizeof(s->daemon.sock_path),
			 "%s/userland.%u.sock", s->env.run_dir, TEST_MINOR_TAPE);

		const char *binary = "vtltape";
		char binary_path[256];
		pid_t pid = fork();
		if (pid == 0) {
			setenv("MHVTL_BACKEND", "userland", 1);
			setenv("MHVTL_CONFIG_PATH", s->env.conf_dir, 1);
			setenv("MHVTL_HOME_PATH", s->env.home_dir, 1);
			setenv("MHVTL_LOCK_PATH", s->env.lock_dir, 1);
			setenv("MHVTL_RUN_PATH", s->env.run_dir, 1);
			if (harness_find_binary(binary, binary_path, sizeof(binary_path)) == 0) {
				const char *local_lib_dir = harness_local_lib_dir(binary_path);
				if (local_lib_dir) {
					const char *old_ld = getenv("LD_LIBRARY_PATH");
					char ld[512];
					snprintf(ld, sizeof(ld), "%s%s%s",
						 local_lib_dir,
						 old_ld ? ":" : "",
						 old_ld ? old_ld : "");
					setenv("LD_LIBRARY_PATH", ld, 1);
				}
			}
			char minor_str[16];
			snprintf(minor_str, sizeof(minor_str), "%u", TEST_MINOR_TAPE);
			char *argv[] = { (char *)binary, "-F", "-q", minor_str, NULL };
			if (harness_find_binary(binary, binary_path, sizeof(binary_path)) == 0)
				execv(binary_path, argv);
			_exit(127);
		}
		s->daemon.pid = pid;
		num_slots++;
	}

	/* Phase 2: wait for ALL sockets to appear (parallel wait) */
	for (int attempt = 0; attempt < 100; attempt++) {
		int all_ready = 1;
		for (int i = 0; i < num_slots; i++) {
			if (slots[i].daemon.sock_fd >= 0) continue;
			if (access(slots[i].daemon.sock_path, F_OK) != 0) {
				all_ready = 0;
				continue;
			}
			/* Socket appeared — connect */
			slots[i].daemon.sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
			struct sockaddr_un addr;
			memset(&addr, 0, sizeof(addr));
			addr.sun_family = AF_UNIX;
			strncpy(addr.sun_path, slots[i].daemon.sock_path,
				sizeof(addr.sun_path) - 1);
			if (connect(slots[i].daemon.sock_fd,
				    (struct sockaddr *)&addr, sizeof(addr)) < 0) {
				close(slots[i].daemon.sock_fd);
				slots[i].daemon.sock_fd = -1;
				all_ready = 0;
			}
		}
		if (all_ready) break;
		usleep(100000);  /* 100ms */
	}

	/* Phase 3: clear UA + load tape for each daemon */
	for (int i = 0; i < num_slots; i++) {
		struct drive_slot *s = &slots[i];
		if (s->daemon.sock_fd < 0) continue;

		harness_clear_ua(&s->daemon);

		/* Load tape via message queue */
		/* Set the right QKEY for this slot */
		char qkey[32];
		snprintf(qkey, sizeof(qkey), "0x%x",
			 (unsigned)(MHVTL_QKEY_BASE + getpid() * 100 + i));
		setenv("MHVTL_QKEY", qkey, 1);

		if (harness_load_tape(TEST_MINOR_TAPE, s->dt->media_barcode) == 0) {
			/* Some drive personalities take a few seconds to report ready
			 * after accepting a media load. */
			for (int j = 0; j < 100; j++) {
				uint8_t cdb[6] = { 0x00 };
				struct scsi_result r;
				harness_cdb(&s->daemon, cdb, 6, &r);
				if (r.sam_stat == SAM_STAT_GOOD) {
					s->ready = 1;
					break;
				}
				usleep(100000);
			}
		}
	}
}

static void teardown_all(void)
{
	for (int i = 0; i < num_slots; i++) {
		/* Set the right QKEY for cleanup */
		char qkey[32];
		snprintf(qkey, sizeof(qkey), "0x%x",
			 (unsigned)(MHVTL_QKEY_BASE + getpid() * 100 + i));
		setenv("MHVTL_QKEY", qkey, 1);

		harness_stop(&slots[i].daemon);
		gen_cleanup_env(&slots[i].env);
	}
	num_slots = 0;
}

static struct drive_slot *find_slot(const char *name)
{
	for (int i = 0; i < num_slots; i++)
		if (strcmp(slots[i].dt->name, name) == 0)
			return &slots[i];
	return NULL;
}

/* ---- Tests ---- */

#define TEST_BLOCK_SIZE 4096

void test_write_read_verify(void)
{
	setup_all();

	uint8_t write_buf[TEST_BLOCK_SIZE];
	uint8_t read_buf[TEST_BLOCK_SIZE];

	for (int i = 0; i < TEST_BLOCK_SIZE; i++)
		write_buf[i] = (uint8_t)(0xa5 ^ (i & 0xff));

	for (int i = 0; i < num_slots; i++) {
		struct drive_slot *s = &slots[i];
		TEST_CASE(s->dt->name);
		struct scsi_result r;

		if (!s->ready) {
			TEST_CHECK_(0, "%s: not ready", s->dt->name);
			continue;
		}

		/* WRITE */
		int rc = harness_tape_write(&s->daemon, write_buf,
					    TEST_BLOCK_SIZE, &r);
		TEST_CHECK_(rc == 0 && r.sam_stat == SAM_STAT_GOOD,
			    "%s: WRITE sam_stat=%d", s->dt->name, r.sam_stat);
		if (r.sam_stat != SAM_STAT_GOOD) continue;

		/* WRITE FILEMARK */
		harness_tape_write_fm(&s->daemon, 1, &r);

		/* REWIND */
		rc = harness_tape_rewind(&s->daemon, &r);
		TEST_CHECK_(rc == 0 && r.sam_stat == SAM_STAT_GOOD,
			    "%s: REWIND", s->dt->name);

		/* READ */
		memset(read_buf, 0, sizeof(read_buf));
		rc = harness_tape_read(&s->daemon, read_buf,
				       TEST_BLOCK_SIZE, &r);
		TEST_CHECK_(rc == 0 && r.sam_stat == SAM_STAT_GOOD,
			    "%s: READ data_len=%u", s->dt->name, r.data_len);

		/* VERIFY */
		if (r.sam_stat == SAM_STAT_GOOD && r.data_len > 0) {
			int match = (memcmp(write_buf, read_buf, TEST_BLOCK_SIZE) == 0);
			TEST_CHECK_(match, "%s: data verify", s->dt->name);
		}
	}

	teardown_all();
}

void test_multi_block(void)
{
	setup_all();

	const char *drives[] = { "LTO-8", "T10000B", "SDLT600", "AIT-4", "3592-E06", NULL };
	uint8_t b1[512], b2[1024], b3[2048], rb[2048];
	memset(b1, 0x11, sizeof(b1));
	memset(b2, 0x22, sizeof(b2));
	memset(b3, 0x33, sizeof(b3));

	for (const char **name = drives; *name; name++) {
		struct drive_slot *s = find_slot(*name);
		if (!s || !s->ready) continue;
		TEST_CASE(s->dt->name);
		struct scsi_result r;

		harness_tape_write(&s->daemon, b1, sizeof(b1), &r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "%s: write b1", *name);
		harness_tape_write(&s->daemon, b2, sizeof(b2), &r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "%s: write b2", *name);
		harness_tape_write(&s->daemon, b3, sizeof(b3), &r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD, "%s: write b3", *name);
		harness_tape_write_fm(&s->daemon, 1, &r);
		harness_tape_rewind(&s->daemon, &r);

		memset(rb, 0, sizeof(rb));
		harness_tape_read(&s->daemon, rb, sizeof(b1), &r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD &&
			    memcmp(rb, b1, sizeof(b1)) == 0,
			    "%s: verify b1", *name);

		memset(rb, 0, sizeof(rb));
		harness_tape_read(&s->daemon, rb, sizeof(b2), &r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD &&
			    memcmp(rb, b2, sizeof(b2)) == 0,
			    "%s: verify b2", *name);

		memset(rb, 0, sizeof(rb));
		harness_tape_read(&s->daemon, rb, sizeof(b3), &r);
		TEST_CHECK_(r.sam_stat == SAM_STAT_GOOD &&
			    memcmp(rb, b3, sizeof(b3)) == 0,
			    "%s: verify b3", *name);
	}

	teardown_all();
}

TEST_LIST = {
	{ "write_read_verify", test_write_read_verify },
	{ "multi_block",       test_multi_block },
	{ NULL, NULL }
};
