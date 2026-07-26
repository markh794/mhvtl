/*
 * Userland transport backend for mhvtl
 *
 * No kernel modules. Uses a unix socketpair to exchange SCSI
 * commands with a test harness (or any external process).
 *
 * The socket path is: /var/run/mhvtl/userland.<minor>.sock
 * The test harness connects as a client; the daemon listens.
 *
 * Wire protocol: same as tcmu_proto.h messages but simpler —
 * we reuse MSG_CDB, MSG_CDB_RESP, MSG_DATA_REQ, MSG_DATA_OUT.
 *
 * Build: make USERLAND=1  (or TCMU=1 also includes it)
 *
 * Copyright (C) 2026 mhvtl contributors
 * License: GPLv2+
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>

#include "vtl_common.h"
#include "mhvtl_scsi.h"
#include "logging.h"
#include "vtllib.h"
#include "transport.h"
#include "tcmu_proto.h"

#define MAX_UL_DEVICES 64

struct ul_dev_state {
	int listen_fd;     /* listening socket */
	int conn_fd;       /* connected harness */
	int active;
	char sock_path[128];
};

static struct ul_dev_state ul_devices[MAX_UL_DEVICES];

static int ul_send(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n <= 0) return -1;
		p += n; len -= n;
	}
	return 0;
}

static int ul_recv(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n <= 0) return -1;
		p += n; len -= n;
	}
	return 0;
}

/*
 * open: Create a listening unix socket for the test harness to connect.
 * Blocks until a harness connects (or returns immediately for add_lu).
 */
static int ul_open(unsigned minor, struct mhvtl_ctl *ctl)
{
	struct ul_dev_state *state;
	struct sockaddr_un addr;

	if (minor >= MAX_UL_DEVICES)
		return -1;

	state = &ul_devices[minor];
	if (state->active)
		return -1;

	/* Socket path: use MHVTL_RUN_PATH env var if set.
	 * Allows tests to run without root and in parallel. */
	{
		const char *run_path = getenv("MHVTL_RUN_PATH");
		if (!run_path || !run_path[0])
			run_path = "/var/run/mhvtl";
		snprintf(state->sock_path, sizeof(state->sock_path),
			 "%s/userland.%u.sock", run_path, minor);
		mkdir(run_path, 0755);
	}
	unlink(state->sock_path);

	state->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (state->listen_fd < 0) {
		MHVTL_ERR("socket: %s", strerror(errno));
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, state->sock_path, sizeof(addr.sun_path) - 1);

	if (bind(state->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		MHVTL_ERR("bind %s: %s", state->sock_path, strerror(errno));
		close(state->listen_fd);
		return -1;
	}

	listen(state->listen_fd, 1);
	state->conn_fd = -1;
	state->active = 1;

	MHVTL_LOG("Userland transport listening on %s", state->sock_path);
	return (int)minor;
}

/*
 * poll_cmd: Check for a SCSI command from the test harness.
 * If no harness connected yet, try to accept one (non-blocking).
 */
static int ul_poll_cmd(int handle, struct mhvtl_header *hdr)
{
	struct ul_dev_state *state;
	struct pollfd pfd;
	int ret;

	if (handle < 0 || handle >= MAX_UL_DEVICES)
		return -1;

	state = &ul_devices[handle];
	if (!state->active)
		return -1;

	/* Accept connection if not yet connected */
	if (state->conn_fd < 0) {
		pfd.fd = state->listen_fd;
		pfd.events = POLLIN;
		ret = poll(&pfd, 1, 0);
		if (ret > 0) {
			state->conn_fd = accept(state->listen_fd, NULL, NULL);
			if (state->conn_fd >= 0)
				MHVTL_DBG(1, "Test harness connected on minor %d",
					   handle);
		}
		return VTL_IDLE;
	}

	/* Check for incoming CDB */
	pfd.fd = state->conn_fd;
	pfd.events = POLLIN;
	ret = poll(&pfd, 1, 0);
	if (ret <= 0)
		return VTL_IDLE;

	/* Read MSG_CDB from harness */
	struct tcmu_msg_cdb msg;
	if (ul_recv(state->conn_fd, &msg, sizeof(msg)) < 0) {
		MHVTL_DBG(1, "Harness disconnected");
		close(state->conn_fd);
		state->conn_fd = -1;
		return VTL_IDLE;
	}

	if (msg.hdr.type != MSG_CDB)
		return VTL_IDLE;

	memcpy(hdr->cdb, msg.cdb, MAX_COMMAND_SIZE);
	hdr->serialNo = msg.cmd_id;

	return VTL_QUEUE_CMD;
}

/*
 * get_data: Receive DATA-OUT from the test harness (for WRITE commands).
 */
static int ul_get_data(int handle, struct mhvtl_ds *ds)
{
	struct ul_dev_state *state;

	if (handle < 0 || handle >= MAX_UL_DEVICES)
		return 0;

	state = &ul_devices[handle];
	if (state->conn_fd < 0)
		return 0;

	/* Send DATA_REQ to harness */
	struct tcmu_msg_data_req req;
	memset(&req, 0, sizeof(req));
	req.hdr.type = MSG_DATA_REQ;
	req.hdr.data_len = sizeof(req) - sizeof(req.hdr);
	req.cmd_id = ds->serialNo;
	req.data_len = ds->sz;

	if (ul_send(state->conn_fd, &req, sizeof(req)) < 0)
		return 0;

	/* Receive DATA_OUT from harness */
	struct tcmu_msg_data_out dout;
	if (ul_recv(state->conn_fd, &dout, sizeof(dout)) < 0)
		return 0;

	if (dout.hdr.type != MSG_DATA_OUT || dout.data_len == 0)
		return 0;

	uint32_t to_read = dout.data_len;
	if (to_read > (uint32_t)ds->sz)
		to_read = ds->sz;

	if (ul_recv(state->conn_fd, ds->data, to_read) < 0)
		return 0;

	return (int)to_read;
}

/*
 * put_data: Send command response + DATA-IN to the test harness.
 */
static void ul_put_data(int handle, struct mhvtl_ds *ds)
{
	struct ul_dev_state *state;

	if (handle < 0 || handle >= MAX_UL_DEVICES)
		return;

	state = &ul_devices[handle];
	if (state->conn_fd < 0)
		return;

	struct tcmu_msg_cdb_resp resp;
	memset(&resp, 0, sizeof(resp));
	resp.hdr.type = MSG_CDB_RESP;
	resp.cmd_id = ds->serialNo;
	resp.sam_stat = ds->sam_stat;
	resp.data_len = ds->sz;

	if (ds->sense_buf)
		memcpy(resp.sense, ds->sense_buf, TCMU_MAX_SENSE_SIZE);

	resp.hdr.data_len = sizeof(resp) - sizeof(resp.hdr) + ds->sz;

	ul_send(state->conn_fd, &resp, sizeof(resp));
	if (ds->sz > 0 && ds->data)
		ul_send(state->conn_fd, ds->data, ds->sz);

	ds->sam_stat = 0;
}

static pid_t ul_add_lu(unsigned minor, struct mhvtl_ctl *ctl)
{
	pid_t pid = fork();
	if (pid == 0) _exit(0);
	return pid > 0 ? pid : 0;
}

static void ul_remove_lu(int handle, struct mhvtl_ctl *ctl) {}

static void ul_close(int handle)
{
	if (handle < 0 || handle >= MAX_UL_DEVICES)
		return;

	struct ul_dev_state *state = &ul_devices[handle];
	if (!state->active)
		return;

	if (state->conn_fd >= 0) close(state->conn_fd);
	if (state->listen_fd >= 0) close(state->listen_fd);
	unlink(state->sock_path);
	state->active = 0;
}

static struct vtl_transport userland_transport = {
	.name      = "userland",
	.open      = ul_open,
	.poll_cmd  = ul_poll_cmd,
	.get_data  = ul_get_data,
	.put_data  = ul_put_data,
	.add_lu    = ul_add_lu,
	.remove_lu = ul_remove_lu,
	.close     = ul_close,
};

struct vtl_transport *transport_userland_init(void)
{
	return &userland_transport;
}
