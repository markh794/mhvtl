/*
 * scsi_harness.h — Shared test infrastructure for mhvtl userland tests.
 *
 * Provides: daemon start/stop, CDB send/recv, tape load via vtlcmd IPC.
 * Used by all test_*.c files that exercise daemons via the userland transport.
 *
 * Include AFTER acutest.h.
 */

#ifndef _MHVTL_SCSI_HARNESS_H
#define _MHVTL_SCSI_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <poll.h>
#include <fcntl.h>
#include <stddef.h>

#include "vtl_common.h"
#include "mhvtl_scsi.h"
#include "tcmu_proto.h"
#include "q.h"

/* ---- Socket I/O ---- */

#define HARNESS_IO_TIMEOUT_MS 2000
#define HARNESS_READY_RETRIES 50

static int harness_wait_fd(int fd, short events, int timeout_ms)
{
	struct pollfd pfd;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = fd;
	pfd.events = events;

	for (;;) {
		int rc = poll(&pfd, 1, timeout_ms);

		if (rc > 0) {
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
				return -1;
			if (pfd.revents & events)
				return 0;
		} else if (rc == 0) {
			errno = ETIMEDOUT;
			return -1;
		} else if (errno != EINTR) {
			return -1;
		}
	}
}

static int harness_send(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		if (harness_wait_fd(fd, POLLOUT, HARNESS_IO_TIMEOUT_MS) < 0)
			return -1;
		ssize_t n = write(fd, p, len);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n <= 0) return -1;
		p += n; len -= n;
	}
	return 0;
}

static int harness_recv(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len > 0) {
		if (harness_wait_fd(fd, POLLIN, HARNESS_IO_TIMEOUT_MS) < 0)
			return -1;
		ssize_t n = read(fd, p, len);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n <= 0) return -1;
		p += n; len -= n;
	}
	return 0;
}

/* ---- SCSI Result ---- */

#define HARNESS_MAX_DATA (256 * 1024)

struct scsi_result {
	uint8_t  sam_stat;
	uint8_t  sense[TCMU_MAX_SENSE_SIZE];
	uint32_t data_len;
	uint8_t  *data;         /* caller-provided or static */
};

/* test_env struct — defined here so scsi_harness.h is self-contained.
 * device_conf_gen.h provides functions to populate it. */
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

/* ---- Daemon handle ---- */

struct harness_daemon {
	pid_t pid;
	int   sock_fd;
	int   minor;
	char  sock_path[256];
};

static uint64_t harness_serial = 1;

static int harness_find_binary(const char *name, char *path, size_t path_sz)
{
	const char *candidates[] = {
		"../usr/bin/",
		"usr/bin/",
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

static const char *harness_local_lib_dir(const char *binary_path)
{
	if (strncmp(binary_path, "../usr/bin/", 11) == 0)
		return "../usr";
	if (strncmp(binary_path, "usr/bin/", 8) == 0)
		return "usr";
	return NULL;
}

/* ---- Send a SCSI CDB ---- */

static int harness_send_cdb(struct harness_daemon *d, const uint8_t *cdb,
			    int cdb_len, struct scsi_result *result,
			    uint8_t *data_buf, size_t data_buf_sz)
{
	struct tcmu_msg_cdb msg;
	memset(&msg, 0, sizeof(msg));
	msg.hdr.type = MSG_CDB;
	msg.hdr.data_len = sizeof(msg) - sizeof(msg.hdr);
	msg.cmd_id = harness_serial++;
	memcpy(msg.cdb, cdb, cdb_len > TCMU_MAX_CDB_SIZE ? TCMU_MAX_CDB_SIZE : cdb_len);

	if (harness_send(d->sock_fd, &msg, sizeof(msg)) < 0)
		return -1;

	struct tcmu_msg_cdb_resp resp;
	if (harness_recv(d->sock_fd, &resp, sizeof(resp)) < 0)
		return -1;

	if (resp.hdr.type != MSG_CDB_RESP)
		return -1;

	memset(result, 0, sizeof(*result));
	result->sam_stat = resp.sam_stat;
	memcpy(result->sense, resp.sense, sizeof(result->sense));
	result->data_len = resp.data_len;
	result->data = data_buf;

	if (resp.data_len > 0 && data_buf) {
		uint32_t to_read = resp.data_len;
		if (to_read > data_buf_sz) to_read = data_buf_sz;
		if (harness_recv(d->sock_fd, data_buf, to_read) < 0)
			return -1;
		/* Drain excess */
		if (resp.data_len > to_read) {
			uint8_t junk[4096];
			uint32_t rem = resp.data_len - to_read;
			while (rem > 0) {
				uint32_t chunk = rem > sizeof(junk) ? sizeof(junk) : rem;
				harness_recv(d->sock_fd, junk, chunk);
				rem -= chunk;
			}
		}
	} else if (resp.data_len > 0) {
		/* No buffer, drain all data */
		uint8_t junk[4096];
		uint32_t rem = resp.data_len;
		while (rem > 0) {
			uint32_t chunk = rem > sizeof(junk) ? sizeof(junk) : rem;
			harness_recv(d->sock_fd, junk, chunk);
			rem -= chunk;
		}
	}

	return 0;
}

/*
 * Send a CDB that includes DATA-OUT (e.g. WRITE).
 * The daemon will send MSG_DATA_REQ, we reply with MSG_DATA_OUT,
 * then read the final MSG_CDB_RESP.
 */
static int harness_send_cdb_with_data_out(struct harness_daemon *d,
					  const uint8_t *cdb, int cdb_len,
					  const uint8_t *write_data, uint32_t write_len,
					  struct scsi_result *result)
{
	struct tcmu_msg_cdb msg;
	memset(&msg, 0, sizeof(msg));
	msg.hdr.type = MSG_CDB;
	msg.hdr.data_len = sizeof(msg) - sizeof(msg.hdr);
	msg.cmd_id = harness_serial++;
	memcpy(msg.cdb, cdb, cdb_len > TCMU_MAX_CDB_SIZE ? TCMU_MAX_CDB_SIZE : cdb_len);

	if (harness_send(d->sock_fd, &msg, sizeof(msg)) < 0)
		return -1;

	/* Read response — might be DATA_REQ or CDB_RESP */
	struct tcmu_msg_hdr resp_hdr;
	if (harness_recv(d->sock_fd, &resp_hdr, sizeof(resp_hdr)) < 0)
		return -1;

	if (resp_hdr.type == MSG_DATA_REQ) {
		/* Daemon wants our write data */
		struct tcmu_msg_data_req dreq;
		memcpy(&dreq.hdr, &resp_hdr, sizeof(resp_hdr));
		if (harness_recv(d->sock_fd,
				 ((uint8_t *)&dreq) + sizeof(resp_hdr),
				 sizeof(dreq) - sizeof(resp_hdr)) < 0)
			return -1;

		uint32_t send_len = dreq.data_len;
		if (send_len > write_len) send_len = write_len;

		struct tcmu_msg_data_out dout;
		memset(&dout, 0, sizeof(dout));
		dout.hdr.type = MSG_DATA_OUT;
		dout.hdr.data_len = sizeof(dout) - sizeof(dout.hdr) + send_len;
		dout.cmd_id = dreq.cmd_id;
		dout.data_len = send_len;

		if (harness_send(d->sock_fd, &dout, sizeof(dout)) < 0)
			return -1;
		if (send_len > 0 && harness_send(d->sock_fd, write_data, send_len) < 0)
			return -1;

		/* Now read the actual CDB_RESP */
		if (harness_recv(d->sock_fd, &resp_hdr, sizeof(resp_hdr)) < 0)
			return -1;
	}

	if (resp_hdr.type != MSG_CDB_RESP)
		return -1;

	struct tcmu_msg_cdb_resp resp;
	memcpy(&resp.hdr, &resp_hdr, sizeof(resp_hdr));
	if (harness_recv(d->sock_fd,
			 ((uint8_t *)&resp) + sizeof(resp_hdr),
			 sizeof(resp) - sizeof(resp_hdr)) < 0)
		return -1;

	memset(result, 0, sizeof(*result));
	result->sam_stat = resp.sam_stat;
	memcpy(result->sense, resp.sense, sizeof(result->sense));
	result->data_len = resp.data_len;

	/* Drain any DATA-IN */
	if (resp.data_len > 0) {
		uint8_t junk[4096];
		uint32_t rem = resp.data_len;
		while (rem > 0) {
			uint32_t chunk = rem > sizeof(junk) ? sizeof(junk) : rem;
			harness_recv(d->sock_fd, junk, chunk);
			rem -= chunk;
		}
	}

	return 0;
}

/* ---- Tape I/O helpers ---- */

/* WRITE(6): write data to tape. Variable block mode. */
static int harness_tape_write(struct harness_daemon *d,
			      const uint8_t *data, uint32_t len,
			      struct scsi_result *r)
{
	uint8_t cdb[6] = {
		0x0a,                       /* WRITE(6) */
		0x00,                       /* fixed=0 (variable block) */
		(len >> 16) & 0xff,
		(len >> 8) & 0xff,
		len & 0xff,
		0x00
	};
	return harness_send_cdb_with_data_out(d, cdb, 6, data, len, r);
}

/* READ(6): read data from tape. Variable block mode. */
static int harness_tape_read(struct harness_daemon *d,
			     uint8_t *buf, uint32_t len,
			     struct scsi_result *r)
{
	uint8_t cdb[6] = {
		0x08,                       /* READ(6) */
		0x00,                       /* fixed=0 (variable block) */
		(len >> 16) & 0xff,
		(len >> 8) & 0xff,
		len & 0xff,
		0x00
	};
	return harness_send_cdb(d, cdb, 6, r, buf, len);
}

/* Convenience: send CDB with small static buffer */
static uint8_t harness_data_buf[4096];

static int harness_cdb(struct harness_daemon *d, const uint8_t *cdb,
		       int cdb_len, struct scsi_result *result)
{
	return harness_send_cdb(d, cdb, cdb_len, result,
				harness_data_buf, sizeof(harness_data_buf));
}

/* REWIND */
static int harness_tape_rewind(struct harness_daemon *d, struct scsi_result *r)
{
	uint8_t cdb[6] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
	return harness_cdb(d, cdb, 6, r);
}

/* WRITE FILEMARKS */
static int harness_tape_write_fm(struct harness_daemon *d, int count,
				 struct scsi_result *r)
{
	uint8_t cdb[6] = {
		0x10,
		0x00,
		(count >> 16) & 0xff,
		(count >> 8) & 0xff,
		count & 0xff,
		0x00
	};
	return harness_cdb(d, cdb, 6, r);
}

/* ---- Daemon start/stop ---- */

/*
 * Start a daemon in an isolated test environment.
 * If env is NULL, uses default system paths (requires root).
 * If env is set, uses the test_env paths (no root needed).
 */
static int harness_start_env(struct harness_daemon *d, unsigned minor,
			     const char *type, const struct test_env *env)
{
	memset(d, 0, sizeof(*d));
	d->minor = minor;
	d->pid = -1;
	d->sock_fd = -1;

	const char *run_dir = env ? env->run_dir : "/var/run/mhvtl";
	const char *lock_dir = env ? env->lock_dir : "/var/lock/mhvtl";

	snprintf(d->sock_path, sizeof(d->sock_path),
		 "%s/userland.%u.sock", run_dir, minor);

	mkdir(run_dir, 0755);
	mkdir(lock_dir, 0755);
	unlink(d->sock_path);

	{
		char lock[256];
		snprintf(lock, sizeof(lock), "%s/mhvtl%u", lock_dir, minor);
		unlink(lock);
	}

	const char *binary = strstr(type, "library") ? "vtllibrary" : "vtltape";
	char binary_path[256];
	const char *local_lib_dir;

	if (harness_find_binary(binary, binary_path, sizeof(binary_path)) < 0)
		return -1;
	local_lib_dir = harness_local_lib_dir(binary_path);

	/* Set unique queue key BEFORE fork so both parent and child see it */
	if (env) {
		char qkey[32];
		snprintf(qkey, sizeof(qkey), "0x%x",
			 (unsigned)(MHVTL_QKEY_BASE + getpid()));
		setenv("MHVTL_QKEY", qkey, 1);
	}

	pid_t pid = fork();
	if (pid == 0) {
		setenv("MHVTL_BACKEND", "userland", 1);
		if (local_lib_dir) {
			/* Ensure shared library is found in local build */
			const char *old_ld = getenv("LD_LIBRARY_PATH");
			char ld_path[512];
			snprintf(ld_path, sizeof(ld_path), "%s%s%s",
				 local_lib_dir,
				 old_ld ? ":" : "", old_ld ? old_ld : "");
			setenv("LD_LIBRARY_PATH", ld_path, 1);
		}
		if (env) {
			setenv("MHVTL_CONFIG_PATH", env->conf_dir, 1);
			setenv("MHVTL_HOME_PATH", env->home_dir, 1);
			setenv("MHVTL_LOCK_PATH", env->lock_dir, 1);
			setenv("MHVTL_RUN_PATH", env->run_dir, 1);
		}
		char minor_str[16];
		snprintf(minor_str, sizeof(minor_str), "%u", minor);
		char *argv[] = { (char *)binary, "-F", "-q", minor_str, NULL };
		execv(binary_path, argv);
		_exit(127);
	}

	d->pid = pid;

	for (int i = 0; i < 100; i++) {
		if (access(d->sock_path, F_OK) == 0)
			break;
		if (waitpid(pid, NULL, WNOHANG) == pid) {
			d->pid = -1;
			return -1;
		}
		usleep(200000);
	}

	if (access(d->sock_path, F_OK) != 0)
		return -1;

	d->sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, d->sock_path, sizeof(addr.sun_path) - 1);

	for (int i = 0; i < 10; i++) {
		if (connect(d->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			struct scsi_result ready;
			uint8_t tur[6] = { 0x00 };

			for (int attempt = 0; attempt < HARNESS_READY_RETRIES; attempt++) {
				if (harness_send_cdb(d, tur, 6, &ready, NULL, 0) == 0)
					return 0;
				usleep(100000);
			}
			break;
		}
		usleep(200000);
	}

	return -1;
}

/* Backward compat: start with default system paths */
static int harness_start(struct harness_daemon *d, unsigned minor,
			 const char *type)
{
	return harness_start_env(d, minor, type, NULL);
}

static void harness_stop(struct harness_daemon *d)
{
	if (d->sock_fd >= 0) { close(d->sock_fd); d->sock_fd = -1; }
	if (d->pid > 0) {
		kill(d->pid, SIGTERM);
		usleep(500000);
		kill(d->pid, SIGKILL);
		waitpid(d->pid, NULL, 0);
		d->pid = -1;
	}
	/* Clean up the per-instance message queue */
	{
		key_t qk = QKEY;
		int qid = msgget(qk, 0);
		if (qid >= 0) msgctl(qid, IPC_RMID, NULL);
	}
}

/* ---- Common SCSI helpers ---- */

static void harness_clear_ua(struct harness_daemon *d)
{
	uint8_t cdb[6] = { 0x00 };  /* TEST UNIT READY */
	struct scsi_result r;
	harness_cdb(d, cdb, 6, &r);
}

/* Send INQUIRY, return device type (0x1f mask) or -1 on error */
static int harness_inquiry(struct harness_daemon *d, uint8_t *buf, int bufsz)
{
	uint8_t cdb[6] = { 0x12, 0, 0, 0, bufsz > 255 ? 255 : bufsz, 0 };
	struct scsi_result r;
	if (harness_send_cdb(d, cdb, 6, &r, buf, bufsz) < 0)
		return -1;
	if (r.sam_stat != SAM_STAT_GOOD)
		return -1;
	return buf[0] & 0x1f;
}

/* ---- vtlcmd IPC: send a command to a daemon via message queue ---- */

/*
 * Send a vtlcmd-style command to a daemon and get the response.
 * Examples: "load E01001L8", "unload", "compression zlib"
 * Returns 0 on success, response text in resp_buf.
 */
static int harness_vtlcmd(unsigned minor, const char *cmd,
			  char *resp_buf, size_t resp_sz)
{
	/* Use the same queue key as the daemon.
	 * MHVTL_QKEY env should be set by harness_start_env.
	 * Retry because daemon may not have created the queue yet. */
	key_t qk = QKEY;
	int qid = -1;
	for (int attempt = 0; attempt < 100 && qid < 0; attempt++) {
		qid = msgget(qk, 0);
		if (qid < 0) usleep(100000);
	}
	if (qid < 0)
		return -1;

	struct q_entry s;
	memset(&s, 0, sizeof(s));
	s.rcv_id = minor;
	s.msg.snd_id = VTLCMD_Q;
	strncpy(s.msg.text, cmd, MAXTEXTLEN);

	if (msgsnd(qid, &s, strlen(cmd) + sizeof(long) + 1, 0) < 0)
		return -1;

	struct q_entry r;
	/* Non-blocking receive with retry (short timeout) */
	for (int attempt = 0; attempt < 200; attempt++) {
		if (msgrcv(qid, &r, MAXOBN, VTLCMD_Q, IPC_NOWAIT) >= 0)
			goto got_response;
		usleep(50000);  /* 50ms, total 1s max */
	}
	return -1;
got_response:

	if (resp_buf && resp_sz > 0) {
		strncpy(resp_buf, r.msg.text, resp_sz - 1);
		resp_buf[resp_sz - 1] = '\0';
	}

	return 0;
}

static int harness_vtlcmd_no_response(unsigned minor, const char *cmd)
{
	key_t qk = QKEY;
	int qid = -1;
	struct q_entry s;

	for (int attempt = 0; attempt < 100 && qid < 0; attempt++) {
		qid = msgget(qk, 0);
		if (qid < 0)
			usleep(100000);
	}
	if (qid < 0)
		return -1;

	memset(&s, 0, sizeof(s));
	s.rcv_id = minor;
	s.msg.snd_id = VTLCMD_Q;
	strncpy(s.msg.text, cmd, MAXTEXTLEN);

	if (msgsnd(qid, &s, strlen(cmd) + sizeof(long) + 1, 0) < 0)
		return -1;

	return 0;
}

/* Load a tape via vtlcmd IPC. Returns 0 if "Loaded OK". */
static int harness_load_tape(unsigned minor, const char *barcode)
{
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "load %s", barcode);
	return harness_vtlcmd_no_response(minor, cmd);
}

/* Unload tape via vtlcmd IPC. */
static int harness_unload_tape(unsigned minor)
{
	char resp[256];
	if (harness_vtlcmd(minor, "unload", resp, sizeof(resp)) < 0)
		return -1;
	return 0;
}

/* Enable compression via vtlcmd IPC. */
static int harness_set_compression(unsigned minor, const char *algo)
{
	char cmd[128], resp[256];
	snprintf(cmd, sizeof(cmd), "compression %s", algo);
	return harness_vtlcmd(minor, cmd, resp, sizeof(resp));
}

#endif /* _MHVTL_SCSI_HARNESS_H */
