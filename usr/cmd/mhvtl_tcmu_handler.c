/*
 * mhvtl_tcmu_handler — TCMU handler daemon for mhvtl.
 *
 * Owns all TCMU/configfs/libtcmu complexity. Tape and library
 * daemons connect via unix socket to register devices and
 * exchange SCSI commands.
 *
 * Architecture:
 *   Main thread:  listen socket + netlink (libtcmu master fd)
 *   Per-device:   worker thread relays CDBs between UIO ring
 *                 and connected daemon socket
 *
 * Lifecycle per device:
 *   1. Daemon connects, sends MSG_REGISTER
 *   2. Handler creates configfs backstore (from worker thread)
 *   3. Main thread enables backstore, receives netlink notification
 *   4. Worker thread maps LUN (handles INQUIRY from UIO)
 *   5. Worker thread relays CDBs: UIO ring ↔ daemon socket
 *
 * When a daemon goes away, its worker fails the commands left in the ring
 * and any new ones until another daemon registers. A daemon that
 * registers again for the same minor reuses the backstore and LUN,
 * which LIO cannot re-enable.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include <sys/uio.h>
#define __LINUX_UIO_H
#include <linux/target_core_user.h>
#include <libtcmu.h>
#include <libtcmu_common.h>

#include "vtl_common.h"
#include "mhvtl_scsi.h"
#include "logging.h"
#include "vtllib.h"
#include "tcmu_proto.h"

char mhvtl_driver_name[] = "mhvtl_tcmu_handler";
extern uint8_t verbose;
extern uint8_t debug;

#define CONFIGFS_TARGET "/sys/kernel/config/target"
#define MAX_DEVICES 64

/* Per-device state */
struct handler_device {
	int                    active;
	int                    daemon_fd;   /* socket to tape daemon */
	unsigned               minor;
	int                    dev_type;    /* 1=tape, 8=changer */
	unsigned               channel;
	unsigned               target;
	unsigned               lun;
	char                   bs_name[32];
	struct tcmu_device    *tcmu_dev;    /* set by added() callback */
	pthread_t              thread;
	pthread_mutex_t        lock;
	int                    ready;       /* UIO connected, LUN mapped */
	int                    daemon_lost; /* socket to daemon failed */
	int                    lun_mapped;  /* LUN mapped, scan answered */
};

static struct handler_device devices[MAX_DEVICES];
static struct tcmulib_context *tcmu_ctx;
static int master_fd;
static volatile int running = 1;
static char target_wwn[128];
static char nexus_wwn[128];
static char device_conf[512];

/* ---- Helpers ---- */

static int configfs_write(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -1;
	ssize_t r = write(fd, val, strlen(val));
	int e = errno;
	close(fd);
	if (r < 0) { errno = e; return -1; }
	return 0;
}

static int configfs_read(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	ssize_t r = read(fd, buf, len - 1);
	close(fd);
	if (r < 0) return -1;
	buf[r] = '\0';
	return 0;
}

static int send_msg(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n <= 0) return -1;
		p += n; len -= n;
	}
	return 0;
}

static int recv_msg(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n <= 0) return -1;
		p += n; len -= n;
	}
	return 0;
}

static int drain_msg(int fd, size_t len)
{
	uint8_t junk[4096];

	while (len > 0) {
		size_t n = len < sizeof(junk) ? len : sizeof(junk);
		if (recv_msg(fd, junk, n) < 0)
			return -1;
		len -= n;
	}
	return 0;
}

/* ---- libtcmu callbacks ---- */

static int dev_added(struct tcmu_device *dev)
{
	const char *cfg = tcmu_dev_get_cfgstring(dev);
	unsigned minor;

	if (!cfg || sscanf(cfg, "mhvtl/%u", &minor) != 1)
		return -1;
	if (minor >= MAX_DEVICES)
		return -1;

	struct handler_device *d = &devices[minor];
	pthread_mutex_lock(&d->lock);
	/* Keep the device even with no daemon registered: it outlives the
	 * daemon, and a daemon that registers again reuses it. */
	if (!d->tcmu_dev) {
		d->tcmu_dev = dev;
		MHVTL_LOG("TCMU device added for minor %u: %s", minor, cfg);
	}
	pthread_mutex_unlock(&d->lock);
	return 0;
}

static void dev_removed(struct tcmu_device *dev)
{
	const char *cfg = tcmu_dev_get_cfgstring(dev);
	unsigned minor;
	if (cfg && sscanf(cfg, "mhvtl/%u", &minor) == 1 &&
	    minor < MAX_DEVICES) {
		pthread_mutex_lock(&devices[minor].lock);
		if (devices[minor].tcmu_dev == dev)
			devices[minor].tcmu_dev = NULL;
		pthread_mutex_unlock(&devices[minor].lock);
	}
}

/*
 * LIO writes <dbroot>/pr/aptpl_<vpd_unit_serial> on every PR OUT
 * REGISTER and fails the command if <dbroot>/pr is missing.
 * targetcli creates it; we configure configfs ourselves.
 * Read dbroot back instead of assuming /var/target: the kernel
 * prefers /etc/target when that directory exists.
 */
static void create_pr_dir(void)
{
	char dbroot[256] = "/var/target";
	char path[300];
	int fd = open(CONFIGFS_TARGET "/dbroot", O_RDONLY);

	if (fd >= 0) {
		ssize_t n = read(fd, dbroot, sizeof(dbroot) - 1);
		close(fd);
		if (n > 0) {
			dbroot[n] = '\0';
			dbroot[strcspn(dbroot, "\n")] = '\0';
		}
		if (n <= 0 || !dbroot[0])
			strcpy(dbroot, "/var/target");
	}

	if (mkdir(dbroot, 0700) < 0 && errno != EEXIST)
		MHVTL_ERR("mkdir %s: %s", dbroot, strerror(errno));
	snprintf(path, sizeof(path), "%s/pr", dbroot);
	if (mkdir(path, 0700) < 0 && errno != EEXIST)
		MHVTL_ERR("mkdir %s: %s -- SPC-3 PR OUT will fail with NOT READY",
			  path, strerror(errno));
	else
		MHVTL_LOG("PR state directory %s", path);
}

/*
 * Find the Unit serial number of the "Library:" (changer) or "Drive:"
 * stanza numbered <minor> in device.conf.
 */
static int conf_unit_serial(unsigned minor, int dev_type, char *out, size_t len)
{
	const char *stanza = dev_type == 8 ? "Library:" : "Drive:";
	char line[256], kw[16], *v;
	unsigned n;
	int in_dev = 0, ret = -1;
	FILE *fp = fopen(device_conf, "r");

	if (!fp)
		return -1;
	while (readline(line, sizeof(line), fp)) {
		if (line[strspn(line, " \t\r\n")] == '\0')
			in_dev = 0;
		else if (sscanf(line, "%15s %u", kw, &n) == 2 &&
			 (!strcmp(kw, "Library:") || !strcmp(kw, "Drive:")))
			in_dev = (!strcmp(kw, stanza) && n == minor);
		else if (in_dev && (v = conf_value(line, "Unit serial number"))) {
			snprintf(out, len, "%s", v);
			ret = 0;
			break;
		}
	}
	fclose(fp);
	return ret;
}

/* ---- LUN mapping thread ---- */

struct lun_map_args {
	char bs_path[512];
	char link_path[512];
	volatile int done;
	int result;
};

static void *lun_map_thread(void *arg)
{
	struct lun_map_args *m = arg;
	m->result = symlink(m->bs_path, m->link_path);
	m->done = 1;
	return NULL;
}

/*
 * Set read_len on the current TCMU ring buffer entry before completion.
 *
 * libtcmu's tcmulib_command_complete() never sets rsp.read_len, so the
 * kernel assumes the full iovec was transferred. For tape short-block
 * reads this is wrong — only a portion of the buffer has valid data.
 *
 * We access the ring buffer directly via dev->map to set read_len
 * BEFORE calling tcmulib_command_complete() (which updates cmd_tail
 * and signals the kernel).
 *
 * The kernel only honours rsp.read_len when TCMU_UFLAG_READ_LEN is set
 * in the entry header, and only gathers DATA-IN for a CHECK CONDITION
 * response when it is: without the flag a short read with ILI reaches
 * the initiator as a full buffer of zeros.
 *
 * struct tcmu_device is opaque from the public libtcmu API, but its
 * first member after fd is the map pointer. We use an ABI-compatible
 * partial struct to access it.
 */
struct tcmu_device_abi {
	int fd;
	struct tcmu_mailbox *map;
	/* remaining fields omitted — we only need map */
};

static void set_read_len_on_current_entry(struct tcmu_device *dev,
					  struct tcmulib_cmd *cmd,
					  uint32_t read_len)
{
	struct tcmu_device_abi *d = (struct tcmu_device_abi *)dev;
	struct tcmu_mailbox *mb = d->map;
	struct tcmu_cmd_entry *ent;
	uint32_t tail = mb->cmd_tail;

	ent = (void *)mb + mb->cmdr_off + tail;

	/* Walk past any PAD entries to find the CMD entry (read-only peek) */
	while (ent != (void *)mb + mb->cmdr_off + mb->cmd_head) {
		if (tcmu_hdr_get_op(ent->hdr.len_op) == TCMU_OP_CMD)
			break;
		tail = (tail + tcmu_hdr_get_len(ent->hdr.len_op)) %
		       mb->cmdr_size;
		ent = (void *)mb + mb->cmdr_off + tail;
	}

	if (!(mb->flags & TCMU_MAILBOX_FLAG_CAP_READ_LEN))
		return;

	ent->rsp.read_len = read_len;
	ent->hdr.uflags |= TCMU_UFLAG_READ_LEN;
}

/*
 * Complete a command with NOT READY, LOGICAL UNIT COMMUNICATION FAILURE
 * (02/08/00), the sense LIO itself returns for a broken TCMU device.
 * Linux does not retry it.
 */
static void fail_cmd(struct handler_device *d, struct tcmulib_cmd *cmd)
{
	/* SPC fixed-format sense: response code, key, length, ASC/ASCQ */
	memset(cmd->sense_buf, 0, sizeof(cmd->sense_buf));
	cmd->sense_buf[0] = 0x70;
	cmd->sense_buf[2] = 0x02;	/* NOT READY */
	cmd->sense_buf[7] = 0x0a;
	cmd->sense_buf[12] = 0x08;	/* LU communication failure */
	tcmulib_command_complete(d->tcmu_dev, cmd, TCMU_STS_PASSTHROUGH_ERR);
}

/* Fail every command in the ring; used once the daemon has gone. */
static void fail_batch(struct handler_device *d)
{
	struct tcmulib_cmd *cmd;

	tcmulib_processing_start(d->tcmu_dev);
	while ((cmd = tcmulib_get_next_command(d->tcmu_dev, 0)))
		fail_cmd(d, cmd);
	tcmulib_processing_complete(d->tcmu_dev);
}

/*
 * Shut the daemon socket down without closing it: a worker blocked in
 * recv() on it returns at once, which close() from another thread does
 * not guarantee. Only the worker closes the fd.
 */
static void stop_daemon_socket(struct handler_device *d)
{
	pthread_mutex_lock(&d->lock);
	if (d->daemon_fd >= 0)
		shutdown(d->daemon_fd, SHUT_RDWR);
	pthread_mutex_unlock(&d->lock);
}

/*
 * Relay one batch of CDBs from the UIO ring to the daemon socket.
 * Handles DATA-OUT (write) and DATA-IN (read) transfers.
 */
static void relay_one_batch(struct handler_device *d)
{
	struct tcmulib_cmd *cmd;

	tcmulib_processing_start(d->tcmu_dev);

	while ((cmd = tcmulib_get_next_command(d->tcmu_dev, 0))) {
		struct tcmu_msg_cdb msg;
		memset(&msg, 0, sizeof(msg));
		msg.hdr.type = MSG_CDB;
		msg.hdr.data_len = sizeof(msg) - sizeof(msg.hdr);
		msg.cmd_id = (uint64_t)(uintptr_t)cmd;
		memcpy(msg.cdb, cmd->cdb, TCMU_MAX_CDB_SIZE);

		if (send_msg(d->daemon_fd, &msg, sizeof(msg)) < 0)
			goto lost;

		struct tcmu_msg_hdr resp_hdr;
		if (recv_msg(d->daemon_fd, &resp_hdr, sizeof(resp_hdr)) < 0)
			goto lost;

		if (resp_hdr.type == MSG_DATA_REQ) {
			struct tcmu_msg_data_req dreq;
			memcpy(&dreq.hdr, &resp_hdr, sizeof(resp_hdr));
			if (recv_msg(d->daemon_fd,
				     ((uint8_t *)&dreq) + sizeof(resp_hdr),
				     sizeof(dreq) - sizeof(resp_hdr)) < 0)
				goto lost;

			uint32_t dlen = dreq.data_len;
			if (dlen > TCMU_MAX_DATA_SIZE) dlen = TCMU_MAX_DATA_SIZE;
			uint8_t *buf = malloc(dlen);
			if (!buf) {
				/* The daemon waits for the data; without it the
				 * stream cannot be kept in step. */
				MHVTL_ERR("[%s] no memory for DATA-OUT", d->bs_name);
				goto lost;
			}
			tcmu_memcpy_from_iovec(buf, dlen, cmd->iovec, cmd->iov_cnt);
			struct tcmu_msg_data_out dout;
			dout.hdr.type = MSG_DATA_OUT;
			dout.hdr.data_len = sizeof(dout) - sizeof(dout.hdr) + dlen;
			dout.cmd_id = dreq.cmd_id;
			dout.data_len = dlen;
			if (send_msg(d->daemon_fd, &dout, sizeof(dout)) < 0 ||
			    send_msg(d->daemon_fd, buf, dlen) < 0) {
				free(buf);
				goto lost;
			}
			free(buf);
			if (recv_msg(d->daemon_fd, &resp_hdr, sizeof(resp_hdr)) < 0)
				goto lost;
		}

		if (resp_hdr.type == MSG_CDB_RESP) {
			struct tcmu_msg_cdb_resp resp;
			memcpy(&resp.hdr, &resp_hdr, sizeof(resp_hdr));
			if (recv_msg(d->daemon_fd,
				     ((uint8_t *)&resp) + sizeof(resp_hdr),
				     sizeof(resp) - sizeof(resp_hdr)) < 0)
				goto lost;

			if (resp.data_len > 0) {
				uint8_t *buf = malloc(resp.data_len);
				if (!buf) {
					/* Read past the data to stay in step
					 * with the daemon, and fail the command. */
					MHVTL_ERR("[%s] no memory for DATA-IN",
						  d->bs_name);
					if (drain_msg(d->daemon_fd, resp.data_len) < 0)
						goto lost;
					fail_cmd(d, cmd);
					continue;
				}
				if (recv_msg(d->daemon_fd, buf, resp.data_len) < 0) {
					free(buf);
					goto lost;
				}
				tcmu_memcpy_into_iovec(cmd->iovec, cmd->iov_cnt,
						       buf, resp.data_len);
				free(buf);
			}

			int tcmu_status;
			if (resp.sam_stat == SAM_STAT_CHECK_CONDITION) {
				/*
				 * Pass the daemon's sense through unchanged:
				 * the VALID bit and INFORMATION field carry the
				 * residual the st driver needs for ILI reads.
				 */
				memcpy(cmd->sense_buf, resp.sense,
				       sizeof(cmd->sense_buf) < sizeof(resp.sense) ?
				       sizeof(cmd->sense_buf) : sizeof(resp.sense));
				tcmu_status = TCMU_STS_PASSTHROUGH_ERR;
			} else if (resp.sam_stat == SAM_STAT_GOOD) {
				tcmu_status = TCMU_STS_OK;
			} else {
				tcmu_status = TCMU_STS_PASSTHROUGH_ERR;
			}

			/*
			 * Set read_len before completion so the kernel
			 * knows actual DATA-IN transfer size (for short
			 * block reads where sz < requested).
			 */
			set_read_len_on_current_entry(d->tcmu_dev, cmd,
						      resp.data_len);
			tcmulib_command_complete(d->tcmu_dev, cmd, tcmu_status);
		} else {
			MHVTL_ERR("[%s] unexpected message type %u", d->bs_name,
				  resp_hdr.type);
			goto lost;
		}
	}

	tcmulib_processing_complete(d->tcmu_dev);
	return;

lost:
	/* The relay is sequential, so this is the only command taken
	 * from the ring. Fail it and the rest of the ring; a command left
	 * uncompleted would stall the ring for good, since closing the
	 * UIO device does not complete it. */
	MHVTL_ERR("[%s] lost the daemon socket", d->bs_name);
	d->daemon_lost = 1;
	stop_daemon_socket(d);
	fail_cmd(d, cmd);
	while ((cmd = tcmulib_get_next_command(d->tcmu_dev, 0)))
		fail_cmd(d, cmd);
	tcmulib_processing_complete(d->tcmu_dev);
}

/*
 * With no daemon, fail commands as they arrive until main() stops this
 * worker: when a daemon registers again, or at shutdown once the LUNs
 * are gone. A blocked device would answer BUSY instead, which the
 * initiator retries for as long as the command timeout allows.
 */
static void serve_without_daemon(struct handler_device *d, int dev_fd)
{
	if (running)
		MHVTL_LOG("[%s] No daemon, failing commands until one registers",
			  d->bs_name);
	fail_batch(d);
	while (d->active) {
		struct pollfd pfd = { .fd = dev_fd, .events = POLLIN };
		if (poll(&pfd, 1, 500) <= 0)
			continue;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
		fail_batch(d);
	}
	/* Anything that arrived during the last poll */
	fail_batch(d);
}

/* ---- Per-device worker thread ---- */

static void *device_worker(void *arg)
{
	struct handler_device *d = arg;
	char path[512], value[128];
	int dev_fd;
	int reused = 0;

	MHVTL_LOG("[%s] Worker started", d->bs_name);

	/*
	 * Step 1: Create and configure backstore.
	 */
	snprintf(path, sizeof(path),
		 CONFIGFS_TARGET "/core/user_0/%s", d->bs_name);
	if (mkdir(path, 0755) < 0 && errno != EEXIST) {
		MHVTL_ERR("[%s] mkdir backstore: %s", d->bs_name, strerror(errno));
		goto err;
	}

	/*
	 * A backstore left by an earlier daemon for this minor is still
	 * enabled. LIO accepts only "1" for enable and refuses it on a
	 * configured device (target_dev_enable_store() and
	 * target_configure_device()), so reuse the backstore as it is.
	 */
	snprintf(path, sizeof(path),
		 CONFIGFS_TARGET "/core/user_0/%s/enable", d->bs_name);
	if (configfs_read(path, value, sizeof(value)) == 0 && value[0] == '1') {
		reused = 1;
		MHVTL_LOG("[%s] Backstore already enabled, reusing it",
			  d->bs_name);
		goto wait_uio;
	}

	snprintf(path, sizeof(path),
		 CONFIGFS_TARGET "/core/user_0/%s/control", d->bs_name);
	snprintf(value, sizeof(value), "dev_config=mhvtl/%u",
		 d->minor);
	configfs_write(path, value);
	configfs_write(path, "dev_size=1048576");
	configfs_write(path, "hw_block_size=512");
	snprintf(value, sizeof(value), "dev_type=%d", d->dev_type);
	configfs_write(path, value);

	/*
	 * LIO names its PR state file aptpl_<vpd_unit_serial>. Set it
	 * before the LUN is linked: LIO refuses changes once exported.
	 * INQUIRY VPD pages still come from the daemon.
	 */
	snprintf(path, sizeof(path),
		 CONFIGFS_TARGET "/core/user_0/%s/wwn/vpd_unit_serial",
		 d->bs_name);
	if (conf_unit_serial(d->minor, d->dev_type, value, sizeof(value)) < 0)
		MHVTL_ERR("[%s] no unit serial for minor %u in %s",
			  d->bs_name, d->minor, device_conf);
	else if (strchr(value, '/'))
		/* LIO builds "%s/pr/aptpl_%s" without sanitising the serial */
		MHVTL_ERR("[%s] unit serial '%s' contains '/', not set",
			  d->bs_name, value);
	else if (configfs_write(path, value) < 0)
		MHVTL_ERR("[%s] vpd_unit_serial '%s': %s",
			  d->bs_name, value, strerror(errno));

	MHVTL_LOG("[%s] Backstore configured", d->bs_name);

	/*
	 * Step 2: Enable backstore.
	 * The main thread holds the netlink socket. The enable write
	 * sends a netlink notification which the main thread receives.
	 * We're in a worker thread, so we don't hold the socket —
	 * no deadlock. Same pattern as tcmu-runner.
	 */
	snprintf(path, sizeof(path),
		 CONFIGFS_TARGET "/core/user_0/%s/enable", d->bs_name);
	if (configfs_write(path, "1") < 0) {
		MHVTL_ERR("[%s] enable failed: %s", d->bs_name, strerror(errno));
		goto err;
	}

wait_uio:
	/* Wait for main thread to process netlink and set tcmu_dev */
	{
		int waited = 0;
		while (!d->tcmu_dev && waited < 15 && running && d->active) {
			usleep(200000);
			waited++;
		}
	}

	if (!d->tcmu_dev) {
		MHVTL_ERR("[%s] UIO device not discovered", d->bs_name);
		goto err;
	}

	dev_fd = tcmu_dev_get_fd(d->tcmu_dev);
	MHVTL_LOG("[%s] Connected to UIO (dev_fd=%d)", d->bs_name, dev_fd);

	/*
	 * Step 3: Send REGISTER_OK so daemon enters its poll loop.
	 * This must happen BEFORE LUN mapping because LUN mapping
	 * triggers a SCSI bus rescan which sends INQUIRYs that
	 * need to be relayed to the daemon.
	 */
	{
		struct tcmu_msg_hdr ok = { .type = MSG_REGISTER_OK, .data_len = 0 };
		send_msg(d->daemon_fd, &ok, sizeof(ok));
	}

	/* Give daemon a moment to enter its poll loop */
	usleep(500000);

	/*
	 * Step 4: Map LUN.
	 * We handle INQUIRY on the UIO ring while the symlink blocks.
	 * The symlink is done inline because we're in a worker thread —
	 * the main thread continues processing netlink events.
	 */
	if (target_wwn[0]) {
		char lun_dir[512], link_path[512], bs_path[512];

		/* Map device.conf target:lun to a unique LIO LUN index.
		 * Use target * 256 + lun to preserve the topology. */
		unsigned lun_idx = d->target * 256 + d->lun;

		snprintf(lun_dir, sizeof(lun_dir),
			 CONFIGFS_TARGET "/loopback/%s/tpgt_1/lun/lun_%u",
			 target_wwn, lun_idx);
		mkdir(lun_dir, 0755);

		snprintf(link_path, sizeof(link_path), "%s/%s", lun_dir, d->bs_name);
		snprintf(bs_path, sizeof(bs_path),
			 CONFIGFS_TARGET "/core/user_0/%s", d->bs_name);

		/* A reused backstore is still mapped. Keep the LUN, so the
		 * host keeps its st and sg nodes for the drive. If the daemon
		 * died during the scan, the host has no device for the LUN:
		 * map it again so the host scans it again. */
		struct stat lst;
		if (reused && d->lun_mapped &&
		    lstat(link_path, &lst) == 0 && S_ISLNK(lst.st_mode)) {
			MHVTL_LOG("[%s] LUN %u already mapped", d->bs_name, lun_idx);
			goto mapped;
		}

		/* Remove stale symlinks */
		DIR *ld = opendir(lun_dir);
		if (ld) {
			struct dirent *le;
			while ((le = readdir(ld)) != NULL) {
				char lp[512];
				struct stat st;
				snprintf(lp, sizeof(lp), "%s/%s", lun_dir, le->d_name);
				if (lstat(lp, &st) == 0 && S_ISLNK(st.st_mode))
					unlink(lp);
			}
			closedir(ld);
		}

		/* The symlink triggers INQUIRY. We spawn a thread for
		 * the blocking symlink while the worker handles INQUIRY
		 * from the UIO ring (via NOT_HANDLED → LIO SPC emulation).
		 */
		struct lun_map_args lma = {
			.done = 0, .result = -1
		};
		strncpy(lma.bs_path, bs_path, sizeof(lma.bs_path) - 1);
		strncpy(lma.link_path, link_path, sizeof(lma.link_path) - 1);

		pthread_t lun_thread;
		pthread_create(&lun_thread, NULL, lun_map_thread, &lma);

		/* Relay CDBs to daemon while symlink thread blocks.
		 * The daemon is in its poll loop (REGISTER_OK was
		 * sent above + 500ms wait). INQUIRY gets properly
		 * handled by the daemon's processCommand. */
		while (!lma.done) {
			struct pollfd pfd = { .fd = dev_fd, .events = POLLIN };
			int ret = poll(&pfd, 1, 500);
			if (d->daemon_lost)
				fail_batch(d);
			else if (ret > 0)
				relay_one_batch(d);
		}
		pthread_join(lun_thread, NULL);

		if (lma.result == 0)
			MHVTL_LOG("[%s] Mapped LUN %u (target %u, lun %u)",
				   d->bs_name, lun_idx, d->target, d->lun);
		else
			MHVTL_ERR("[%s] LUN mapping failed", d->bs_name);
		d->lun_mapped = lma.result == 0 && !d->daemon_lost;
	}

mapped:
	d->ready = 1;
	MHVTL_LOG("[%s] Device ready — relaying CDBs", d->bs_name);

	/*
	 * Step 4: CDB relay loop.
	 * Read CDB from UIO ring, send to daemon, get response, complete.
	 */
	while (running && d->active && !d->daemon_lost) {
		struct pollfd pfd[2] = {
			{ .fd = dev_fd, .events = POLLIN },
			{ .fd = d->daemon_fd, .events = POLLIN },
		};
		int ret = poll(pfd, 2, 1000);
		if (ret <= 0)
			continue;
		/* The daemon only writes in reply to a CDB, so anything on
		 * its socket between commands means it has gone. */
		if (pfd[1].revents) {
			MHVTL_LOG("[%s] Daemon closed its socket", d->bs_name);
			d->daemon_lost = 1;
			stop_daemon_socket(d);
			break;
		}
		if (pfd[0].revents)
			relay_one_batch(d);
	}

	/* The daemon is gone, or the handler is stopping. Keep the ring
	 * serviced until main() stops this worker: before it starts the
	 * next one, or at shutdown after it has removed the LUNs. */
	if (d->ready && (d->daemon_lost || !running))
		serve_without_daemon(d, dev_fd);

err:
	MHVTL_LOG("[%s] Worker exiting", d->bs_name);
	pthread_mutex_lock(&d->lock);
	if (d->daemon_fd >= 0) {
		close(d->daemon_fd);
		d->daemon_fd = -1;
	}
	pthread_mutex_unlock(&d->lock);
	d->active = 0;
	return NULL;
}

/* ---- Signal handler ---- */

static void sighandler(int sig) { running = 0; }

/* ---- Create shared infrastructure ---- */

static int create_infrastructure(const char *conf_path)
{
	char path[512];
	FILE *fp;
	char line[256];

	/* Parse first NAA from device.conf for target WWN */
	snprintf(device_conf, sizeof(device_conf), "%s", conf_path);
	fp = fopen(conf_path, "r");
	if (!fp) {
		MHVTL_ERR("Cannot open %s", conf_path);
		return -1;
	}

	target_wwn[0] = '\0';
	while (fgets(line, sizeof(line), fp)) {
		char naa[64];
		if (sscanf(line, " NAA: %63s", naa) == 1 && !target_wwn[0]) {
			char *dst = target_wwn;
			dst += sprintf(dst, "naa.");
			for (char *p = naa; *p; p++)
				if (*p != ':') *dst++ = *p;
			*dst = '\0';
			strcpy(nexus_wwn, target_wwn);
			int len = strlen(nexus_wwn);
			if (len >= 2) {
				nexus_wwn[len - 2] = 'f';
				nexus_wwn[len - 1] = 'f';
			}
			break;
		}
	}
	fclose(fp);

	if (!target_wwn[0]) {
		MHVTL_ERR("No NAA found in %s", conf_path);
		return -1;
	}

	/* Create HBA */
	snprintf(path, sizeof(path), CONFIGFS_TARGET "/core/user_0");
	mkdir(path, 0755);

	/* Create loopback target + nexus */
	snprintf(path, sizeof(path), CONFIGFS_TARGET "/loopback");
	mkdir(path, 0755);
	snprintf(path, sizeof(path), CONFIGFS_TARGET "/loopback/%s", target_wwn);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), CONFIGFS_TARGET "/loopback/%s/tpgt_1", target_wwn);
	mkdir(path, 0755);

	snprintf(path, sizeof(path),
		 CONFIGFS_TARGET "/loopback/%s/tpgt_1/nexus", target_wwn);
	{
		char cur[128] = {0};
		int fd = open(path, O_RDONLY);
		if (fd >= 0) { read(fd, cur, sizeof(cur) - 1); close(fd); }
		if (cur[0] == '\0' || cur[0] == '\n')
			configfs_write(path, nexus_wwn);
	}

	MHVTL_LOG("Infrastructure created: target=%s nexus=%s", target_wwn, nexus_wwn);
	return 0;
}

/* ---- Main ---- */

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-d] [-v level] [-F] [-c config_path]\n", prog);
}

int main(int argc, char *argv[])
{
	int opt, foreground = 0, listen_fd, i;
	char conf_path[512] = MHVTL_CONFIG_PATH "/device.conf";
	struct sockaddr_un addr;
	struct sigaction sa;

	setbuf(stdout, NULL);

	while ((opt = getopt(argc, argv, "dv:Fc:")) != -1) {
		switch (opt) {
		case 'd': debug = 1; verbose = 9; foreground = 1; break;
		case 'v': verbose = atoi(optarg); break;
		case 'F': foreground = 1; break;
		case 'c': strncpy(conf_path, optarg, sizeof(conf_path) - 1); break;
		default: usage(argv[0]); exit(1);
		}
	}

	openlog("mhvtl_tcmu_handler", LOG_PID, LOG_DAEMON);

	/* Only run when TCMU backend is configured */
	{
		const char *backend = getenv("MHVTL_BACKEND");
		if (!backend || strcmp(backend, "tcmu") != 0) {
			MHVTL_LOG("Backend is '%s', not 'tcmu' — exiting",
				   backend ? backend : "unset");
			return 0;
		}
	}

	/* Initialize device array */
	for (i = 0; i < MAX_DEVICES; i++) {
		devices[i].daemon_fd = -1;
		pthread_mutex_init(&devices[i].lock, NULL);
	}

	/* Create shared infrastructure */
	if (create_infrastructure(conf_path) < 0)
		exit(1);

	create_pr_dir();

	/* Initialize libtcmu — ALL subtypes use name "mhvtl" */
	struct tcmulib_handler handler = {
		.name     = "mhvtl",
		.subtype  = "mhvtl",  /* prefix match: matches mhvtl10, mhvtl11, etc. */
		.cfg_desc = "mhvtl TCMU handler",
		.added    = dev_added,
		.removed  = dev_removed,
	};

	tcmu_ctx = tcmulib_initialize(&handler, 1);
	if (!tcmu_ctx) {
		MHVTL_ERR("tcmulib_initialize failed");
		exit(1);
	}
	master_fd = tcmulib_get_master_fd(tcmu_ctx);
	MHVTL_LOG("libtcmu initialized (master_fd=%d)", master_fd);

	/* Set up signal handlers */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	/* Create listen socket */
	unlink(TCMU_SOCK_PATH);
	mkdir("/run/mhvtl", 0755);
	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		MHVTL_ERR("socket: %s", strerror(errno));
		exit(1);
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, TCMU_SOCK_PATH, sizeof(addr.sun_path) - 1);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		MHVTL_ERR("bind: %s", strerror(errno));
		exit(1);
	}
	listen(listen_fd, 16);
	MHVTL_LOG("Listening on %s", TCMU_SOCK_PATH);

	if (!foreground && daemon(0, 0) < 0) {
		perror("daemon");
		exit(1);
	}

	/* Main loop: accept connections + process netlink */
	while (running) {
		struct pollfd pfds[2];
		pfds[0].fd = listen_fd;
		pfds[0].events = POLLIN;
		pfds[1].fd = master_fd;
		pfds[1].events = POLLIN;

		int ret = poll(pfds, 2, 1000);
		if (ret < 0) {
			if (errno == EINTR) continue;
			break;
		}

		/* Process netlink events (device add/remove) */
		if (pfds[1].revents & POLLIN)
			tcmulib_master_fd_ready(tcmu_ctx);

		/* Accept new daemon connection */
		if (pfds[0].revents & POLLIN) {
			int client_fd = accept(listen_fd, NULL, NULL);
			if (client_fd < 0) continue;

			/* Read registration message */
			struct tcmu_msg_register reg;
			if (recv_msg(client_fd, &reg, sizeof(reg)) < 0) {
				close(client_fd);
				continue;
			}

			if (reg.hdr.type != MSG_REGISTER ||
			    reg.minor >= MAX_DEVICES) {
				struct tcmu_msg_hdr err = {
					.type = MSG_REGISTER_ERR, .data_len = 0 };
				send_msg(client_fd, &err, sizeof(err));
				close(client_fd);
				continue;
			}

			struct handler_device *d = &devices[reg.minor];

			/* A new daemon for this minor means the old one has
			 * exited. Stop the old worker, which is failing
			 * commands in its place. */
			if (d->thread) {
				d->active = 0;
				stop_daemon_socket(d);
				pthread_join(d->thread, NULL);
				d->thread = 0;
			}

			pthread_mutex_lock(&d->lock);
			d->active = 1;
			d->daemon_fd = client_fd;
			d->minor = reg.minor;
			d->dev_type = reg.dev_type;
			d->channel = reg.channel;
			d->target = reg.target;
			d->lun = reg.lun;
			strncpy(d->bs_name, reg.bs_name, sizeof(d->bs_name) - 1);
			d->ready = 0;
			d->daemon_lost = 0;
			pthread_mutex_unlock(&d->lock);

			MHVTL_LOG("Daemon registered: %s (minor=%u, type=%d, %u:%u:%u)",
				   d->bs_name, d->minor, d->dev_type,
				   d->channel, d->target, d->lun);

			/* Start worker thread */
			int rc = pthread_create(&d->thread, NULL, device_worker, d);
			if (rc != 0) {
				MHVTL_ERR("[%s] cannot start worker: %s",
					  d->bs_name, strerror(rc));
				struct tcmu_msg_hdr err = {
					.type = MSG_REGISTER_ERR, .data_len = 0 };
				send_msg(client_fd, &err, sizeof(err));
				pthread_mutex_lock(&d->lock);
				close(client_fd);
				d->daemon_fd = -1;
				d->active = 0;
				pthread_mutex_unlock(&d->lock);
				d->thread = 0;
			}
		}
	}

	MHVTL_LOG("Shutting down — waiting for workers ...");

	/* Shut the daemon sockets down so worker threads leave their relay
	 * loops. They keep failing commands until the LUNs are removed
	 * below. */
	for (i = 0; i < MAX_DEVICES; i++)
		stop_daemon_socket(&devices[i]);

	/*
	 * Remove the LUNs while the workers still service the rings.
	 * Removing a LUN removes the host's SCSI device, which waits for
	 * every command it has outstanding. A command left in a ring
	 * nobody reads would hold that up until cmd_time_out.
	 */
	if (target_wwn[0]) {
		char path[512];
		for (i = 0; i < MAX_DEVICES; i++) {
			if (!devices[i].bs_name[0])
				continue;

			unsigned lun_idx = devices[i].target * 256 + devices[i].lun;

			/* Remove symlink */
			snprintf(path, sizeof(path),
				 CONFIGFS_TARGET "/loopback/%s/tpgt_1/lun/lun_%u/%s",
				 target_wwn, lun_idx, devices[i].bs_name);
			unlink(path);

			/* Remove LUN dir */
			snprintf(path, sizeof(path),
				 CONFIGFS_TARGET "/loopback/%s/tpgt_1/lun/lun_%u",
				 target_wwn, lun_idx);
			rmdir(path);
		}

		/* Remove TPGT (nexus is auto-removed) */
		snprintf(path, sizeof(path),
			 CONFIGFS_TARGET "/loopback/%s/tpgt_1", target_wwn);
		rmdir(path);

		/* Remove loopback target */
		snprintf(path, sizeof(path),
			 CONFIGFS_TARGET "/loopback/%s", target_wwn);
		rmdir(path);
	}

	/*
	 * Block the backstores before stopping the workers. A command that
	 * still reaches a ring, say for a LUN mapped after the removal
	 * above, gets BUSY instead of waiting in a ring nobody reads:
	 * tcmulib_close() waits for every ring to be empty.
	 */
	for (i = 0; i < MAX_DEVICES; i++) {
		char path[512];
		if (!devices[i].bs_name[0])
			continue;
		snprintf(path, sizeof(path),
			 CONFIGFS_TARGET "/core/user_0/%s/action/block_dev",
			 devices[i].bs_name);
		configfs_write(path, "1");
	}

	/* Join all worker threads — ensures no thread is using
	 * tcmu_dev or configfs when we tear them down */
	for (i = 0; i < MAX_DEVICES; i++)
		devices[i].active = 0;
	for (i = 0; i < MAX_DEVICES; i++) {
		if (devices[i].thread) {
			pthread_join(devices[i].thread, NULL);
			devices[i].thread = 0;
		}
	}

	MHVTL_LOG("All workers stopped, cleaning configfs ...");

	/* Close libtcmu BEFORE configfs teardown — releases UIO fds
	 * which release target_core_user references */
	tcmulib_close(tcmu_ctx);
	tcmu_ctx = NULL;

	/* Disable and remove backstores */
	{
		char path[512];
		for (i = 0; i < MAX_DEVICES; i++) {
			if (!devices[i].bs_name[0])
				continue;

			snprintf(path, sizeof(path),
				 CONFIGFS_TARGET "/core/user_0/%s/enable",
				 devices[i].bs_name);
			configfs_write(path, "0");

			snprintf(path, sizeof(path),
				 CONFIGFS_TARGET "/core/user_0/%s",
				 devices[i].bs_name);
			rmdir(path);
		}

		/* Remove HBA */
		rmdir(CONFIGFS_TARGET "/core/user_0");
	}

	/* Remove the loopback fabric directory — this releases the
	 * tcm_loop module reference so rmmod can succeed. */
	rmdir(CONFIGFS_TARGET "/loopback");

	unlink(TCMU_SOCK_PATH);

	MHVTL_LOG("Shutdown complete — configfs cleaned");
	return 0;
}
