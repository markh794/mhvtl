/*
 * TCMU socket transport backend for mhvtl
 *
 * Connects to the mhvtl_tcmu_handler daemon via unix socket.
 * All TCMU/configfs/libtcmu complexity lives in the handler;
 * this module just sends/receives SCSI commands over a socket.
 *
 * No libtcmu dependency. No configfs manipulation. No forks.
 *
 * Copyright (C) 2026 mhvtl contributors
 * License: GPLv2+
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "vtl_common.h"
#include "mhvtl_scsi.h"
#include "logging.h"
#include "vtllib.h"
#include "transport.h"
#include "tcmu_proto.h"

#define MAX_TCMU_DEVICES 64

struct tcmu_dev_state {
	int sock_fd;
	int active;
	char bs_name[32];
};

static struct tcmu_dev_state tcmu_devices[MAX_TCMU_DEVICES];

/* ---- Socket helpers ---- */

static int sock_send(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		/* MSG_NOSIGNAL: a handler that is going away must give EPIPE,
		 * not a SIGPIPE that kills a daemon with no handler for it */
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n; len -= n;
	}
	return 0;
}

static int sock_recv(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n; len -= n;
	}
	return 0;
}

/*
 * The handler went away (it exited, or the stream is out of step): close
 * the socket so nothing else is read from it, and report it once.
 */
static int lost_handler(struct tcmu_dev_state *state, const char *why)
{
	MHVTL_ERR("%s: lost the TCMU handler (%s), exiting so the service "
		  "restarts and registers again", state->bs_name, why);
	close(state->sock_fd);
	state->sock_fd = -1;
	state->active = 0;
	return VTL_TRANSPORT_LOST;
}

/*
 * open: Connect to the TCMU handler daemon and register this device.
 */
static int tcmu_open(unsigned minor, struct mhvtl_ctl *ctl)
{
	struct tcmu_dev_state *state;
	struct sockaddr_un addr;
	int fd, dev_type;

	if (minor >= MAX_TCMU_DEVICES) {
		MHVTL_ERR("minor %u exceeds MAX_TCMU_DEVICES", minor);
		return -1;
	}

	state = &tcmu_devices[minor];
	if (state->active) {
		MHVTL_ERR("minor %u already open", minor);
		return -1;
	}

	/* Determine device name and type */
	extern char mhvtl_driver_name[];
	if (strstr(mhvtl_driver_name, "library")) {
		snprintf(state->bs_name, sizeof(state->bs_name), "lib%u", minor);
		dev_type = 8;
	} else {
		snprintf(state->bs_name, sizeof(state->bs_name), "tape%u", minor);
		dev_type = 1;
	}

	/* Fill in the registration once; it is resent on each attempt */
	struct tcmu_msg_register reg;
	memset(&reg, 0, sizeof(reg));
	reg.hdr.type = MSG_REGISTER;
	reg.hdr.data_len = sizeof(reg) - sizeof(reg.hdr);
	reg.minor = minor;
	reg.dev_type = dev_type;
	reg.channel = ctl->channel;
	reg.target = ctl->id;
	reg.lun = ctl->lun;
	strncpy(reg.bs_name, state->bs_name, sizeof(reg.bs_name) - 1);

	/*
	 * Connect and register, retrying for up to 30 s: the handler may
	 * still be starting, or, after a handler restart, the old one may
	 * close the connection without answering. Each answer is waited for
	 * at most 60 s (a registration creates and enables the backstore),
	 * so a handler that accepts and then hangs cannot block us for ever.
	 */
	struct timespec now, deadline;
	const struct timeval reg_timeout = { .tv_sec = 60 };
	const struct timeval no_timeout = { 0 };
	int attempts = 0;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += 30;
	fd = -1;
	for (;;) {
		struct tcmu_msg_hdr resp;

		if (attempts++) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (now.tv_sec >= deadline.tv_sec)
				break;
			sleep(1);
		}

		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			MHVTL_ERR("socket: %s", strerror(errno));
			return -1;
		}

		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, TCMU_SOCK_PATH,
			sizeof(addr.sun_path) - 1);

		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			if (attempts == 1) {
				MHVTL_DBG(1, "Waiting for TCMU handler daemon ...");
			}
			close(fd);
			fd = -1;
			continue;
		}

		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
			   &reg_timeout, sizeof(reg_timeout));
		if (sock_send(fd, &reg, sizeof(reg)) < 0 ||
		    sock_recv(fd, &resp, sizeof(resp)) < 0) {
			MHVTL_DBG(1, "TCMU handler closed the connection or did "
				  "not answer, retrying");
			close(fd);
			fd = -1;
			continue;
		}

		if (resp.type != MSG_REGISTER_OK) {
			MHVTL_ERR("Registration rejected for %s", state->bs_name);
			close(fd);
			return -1;
		}
		/* Commands are read only after select() says so: no timeout */
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
			   &no_timeout, sizeof(no_timeout));
		break;
	}

	if (fd < 0) {
		MHVTL_ERR("Cannot register with %s after 30s", TCMU_SOCK_PATH);
		return -1;
	}

	state->sock_fd = fd;
	state->active = 1;

	MHVTL_LOG("Connected to TCMU handler for %s", state->bs_name);
	return (int)minor;
}

/*
 * poll_cmd: Check for a pending SCSI command from the handler.
 */
static int tcmu_poll_cmd(int handle, struct mhvtl_header *hdr)
{
	struct tcmu_dev_state *state;
	struct tcmu_msg_hdr msg_hdr;
	struct tcmu_msg_cdb msg;
	fd_set rfds;
	struct timeval tv;
	int ret;

	if (handle < 0 || handle >= MAX_TCMU_DEVICES)
		return -1;

	state = &tcmu_devices[handle];
	if (!state->active)
		return VTL_TRANSPORT_LOST;

	/* Non-blocking check */
	FD_ZERO(&rfds);
	FD_SET(state->sock_fd, &rfds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;

	ret = select(state->sock_fd + 1, &rfds, NULL, NULL, &tv);
	if (ret <= 0)
		return VTL_IDLE;

	/* Read message header */
	/* EOF when the handler exits: select() keeps reporting it readable */
	if (sock_recv(state->sock_fd, &msg, sizeof(msg)) < 0)
		return lost_handler(state, "socket closed");

	if (msg.hdr.type == MSG_SHUTDOWN)
		return lost_handler(state, "handler shutting down");

	if (msg.hdr.type != MSG_CDB) {
		MHVTL_ERR("unexpected message type %u", msg.hdr.type);
		return lost_handler(state, "protocol error");
	}

	memcpy(hdr->cdb, msg.cdb, MAX_COMMAND_SIZE);
	hdr->serialNo = msg.cmd_id;

	return VTL_QUEUE_CMD;
}

/*
 * get_data: Request DATA-OUT from the handler.
 */
static int tcmu_get_data(int handle, struct mhvtl_ds *ds)
{
	struct tcmu_dev_state *state;

	if (handle < 0 || handle >= MAX_TCMU_DEVICES)
		return 0;

	state = &tcmu_devices[handle];

	MHVTL_DBG(3, "TCMU get_data: requesting %d bytes", ds->sz);

	/* Send DATA_REQ */
	struct tcmu_msg_data_req req;
	memset(&req, 0, sizeof(req));
	req.hdr.type = MSG_DATA_REQ;
	req.hdr.data_len = sizeof(req) - sizeof(req.hdr);
	req.cmd_id = ds->serialNo;
	req.data_len = ds->sz;

	if (sock_send(state->sock_fd, &req, sizeof(req)) < 0)
		return 0;

	/* Receive DATA_OUT */
	struct tcmu_msg_data_out dout;
	if (sock_recv(state->sock_fd, &dout, sizeof(dout)) < 0)
		return 0;

	if (dout.hdr.type != MSG_DATA_OUT || dout.data_len == 0)
		return 0;

	uint32_t to_read = dout.data_len;
	if (to_read > (uint32_t)ds->sz)
		to_read = ds->sz;

	if (sock_recv(state->sock_fd, ds->data, to_read) < 0)
		return 0;

	return (int)to_read;
}

/*
 * put_data: Send command response back to the handler.
 */
static void tcmu_put_data(int handle, struct mhvtl_ds *ds)
{
	struct tcmu_dev_state *state;

	if (handle < 0 || handle >= MAX_TCMU_DEVICES)
		return;

	state = &tcmu_devices[handle];

	struct tcmu_msg_cdb_resp resp;
	memset(&resp, 0, sizeof(resp));
	resp.hdr.type = MSG_CDB_RESP;
	resp.cmd_id = ds->serialNo;
	resp.sam_stat = ds->sam_stat;
	resp.data_len = ds->sz;

	if (ds->sense_buf)
		memcpy(resp.sense, ds->sense_buf, TCMU_MAX_SENSE_SIZE);

	resp.hdr.data_len = sizeof(resp) - sizeof(resp.hdr) + ds->sz;

	/* Send header + DATA-IN */
	sock_send(state->sock_fd, &resp, sizeof(resp));
	if (ds->sz > 0 && ds->data)
		sock_send(state->sock_fd, ds->data, ds->sz);

	if (ds->sam_stat == SAM_STAT_CHECK_CONDITION) {
		uint8_t *s = (uint8_t *)ds->sense_buf;
		MHVTL_DBG(2, "TCMU s/n: (%ld), sz: %d, status: %d "
			      "[%02x %02x %02x]",
			   (unsigned long)ds->serialNo, ds->sz, ds->sam_stat,
			   s[2], s[12], s[13]);
	} else {
		MHVTL_DBG(2, "TCMU s/n: (%ld), sz: %d, status: %d",
			   (unsigned long)ds->serialNo, ds->sz, ds->sam_stat);
	}

	ds->sam_stat = 0;
}

/* ---- add_lu / remove_lu / close ---- */

static pid_t tcmu_add_lu(unsigned minor, struct mhvtl_ctl *ctl)
{
	MHVTL_DBG(1, "TCMU add_lu: no-op for minor %u", minor);
	pid_t pid = fork();
	if (pid == 0) _exit(0);
	return pid > 0 ? pid : 0;
}

static void tcmu_remove_lu(int handle, struct mhvtl_ctl *ctl)
{
	MHVTL_DBG(1, "TCMU remove_lu: no-op");
}

static void tcmu_close(int handle)
{
	if (handle < 0 || handle >= MAX_TCMU_DEVICES)
		return;

	struct tcmu_dev_state *state = &tcmu_devices[handle];
	if (!state->active)
		return;

	MHVTL_DBG(1, "TCMU closing %s", state->bs_name);

	if (state->sock_fd >= 0) {
		close(state->sock_fd);
		state->sock_fd = -1;
	}
	state->active = 0;
}

/* ---- Transport vtable ---- */

static struct vtl_transport tcmu_transport = {
	.name      = "tcmu",
	.open      = tcmu_open,
	.poll_cmd  = tcmu_poll_cmd,
	.get_data  = tcmu_get_data,
	.put_data  = tcmu_put_data,
	.add_lu    = tcmu_add_lu,
	.remove_lu = tcmu_remove_lu,
	.close     = tcmu_close,
};

struct vtl_transport *transport_tcmu_init(void)
{
	return &tcmu_transport;
}
