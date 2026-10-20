/*
 * mhvtl-bay-monitor — libudev raw-disk bay hot-plug monitor (Phase 2, Stage B).
 *
 * A disk bay is a library Import/Export (MAP) slot bound to a stable device
 * node by the bay config (see etc/mhvtl_raw_bays.conf.sample). The Stage-A
 * provisioner (mhvtl-raw-provision) places bays ONCE at startup; this daemon
 * keeps a running library in sync with the physical disks by reacting to
 * insertion/removal events:
 *
 *   - a disk inserted in a configured bay is probed and, if it carries a valid
 *     MVTL cartridge, loaded into that bay's library MAP (place);
 *   - a disk pulled from a configured bay has its cartridge removed from the
 *     library MAP and its home_directory/<barcode> symlink dropped (remove);
 *   - a disk already in an empty bay is probed again when udev reports it
 *     changed, which it does when a program that opened it for writing closes
 *     it. A disk formatted with mhvtl-raw-format while in the bay is placed
 *     that way, without being pulled and reinserted;
 *   - a cartridge the library already holds when a bay's disk is first seen,
 *     placed before the monitor (re)started, is tracked as if the monitor had
 *     placed it, so pulling the disk still removes it.
 *
 * The probe/symlink/load-map and remove logic lives in the reusable raw_bay_ops
 * module (compiled into libvtlscsi) and is shared verbatim with the Stage-A
 * provisioner; this daemon only watches udev and drives raw_bay_place_one() /
 * raw_bay_remove() for the matching bay.
 *
 * MATCHING: the match key for a bay is the udev ID_PATH property. A bay whose
 * by-path binding is "/dev/disk/by-path/<id-path>" matches events carrying
 * ID_PATH == "<id-path>". ID_PATH is present on BOTH add and remove events even
 * though the by-path symlink itself is gone after removal, which is why removal
 * is possible at all — and why the removed cartridge's barcode must be the one
 * we STORED when we placed it (a gone device cannot be probed).
 *
 * STARTUP PRE-CHECK: raw_bay_place_one()/raw_bay_remove() resolve the library
 * home directory via find_media_home_directory(), which exit()s the process if
 * <config_dir>/device.conf is unreadable. A hot-plug event must never be able to
 * kill the running daemon, so device.conf readability is verified once at
 * startup, before the event loop, and the daemon refuses to start without it.
 *
 * Usage:
 *   mhvtl-bay-monitor [--config <path>] [--foreground] [-v|--verbose] [-h|--help]
 *
 * Default config: $MHVTL_CONFIG_PATH/mhvtl_raw_bays.conf (else /etc/mhvtl/...).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <getopt.h>
#include <signal.h>
#include <poll.h>
#include <libudev.h>

#include "raw_bay_conf.h"
#include "raw_bay_ops.h"
#include "cart_backend_raw.h"
#include "logging.h"

/* Every mhvtl program that links libvtlscsi defines this (used by logging). */
char mhvtl_driver_name[] = "mhvtl-bay-monitor";

/* The supported bay binding: a /dev/disk/by-path/<id-path> node. */
#define BY_PATH_PREFIX	"/dev/disk/by-path/"

/*
 * One monitored bay: the parsed bay plus its udev match key and the barcode we
 * last placed from it (needed to remove it once the device is gone and can no
 * longer be probed).
 */
struct bay_mon {
	const struct raw_bay	*bay;
	const char		*match_key;	/* ID_PATH suffix, NULL if unbound */
	char			 resolved[PATH_MAX]; /* realpath(by_path), coldplug fallback */
	char			 placed_barcode[33];
	int			 has_placed;
};

/* SIGINT/SIGTERM only set this; the poll loop checks it and exits cleanly. */
static volatile sig_atomic_t stop_requested;

static void caught_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: mhvtl-bay-monitor [--config <path>] [--foreground] [--verbose]\n"
		"\n"
		"  --config <path>  bay map config (default: "
		"$MHVTL_CONFIG_PATH/mhvtl_raw_bays.conf)\n"
		"  --foreground     run in the foreground (the default)\n"
		"  -v, --verbose    log per-bay probe/place decisions\n"
		"  -h, --help       show this help\n");
}

/*
 * Return the index of the monitored bay matching this event/device, or -1.
 * `id_path` is the udev ID_PATH property (NULL if absent); `devnode` is the
 * resolved device node (NULL if absent). A bay bound by /dev/disk/by-path
 * matches on ID_PATH (works for add AND remove); an unbound bay can only be
 * matched by its resolved devnode, which is a coldplug-only fallback.
 */
static int match_bay(const struct bay_mon *mons, size_t n,
		     const char *id_path, const char *devnode)
{
	for (size_t i = 0; i < n; i++) {
		if (mons[i].match_key) {
			if (id_path && strcmp(id_path, mons[i].match_key) == 0)
				return (int)i;
		} else if (devnode && mons[i].resolved[0] &&
			   strcmp(devnode, mons[i].resolved) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* The bay other than `self` that placed `barcode`, or NULL. */
static const struct bay_mon *owner_of(const struct bay_mon *mons, size_t n,
				      const struct bay_mon *self,
				      const char *barcode)
{
	for (size_t i = 0; i < n; i++)
		if (&mons[i] != self && mons[i].has_placed &&
		    strcmp(mons[i].placed_barcode, barcode) == 0)
			return &mons[i];
	return NULL;
}

/* Place the disk now sitting in this bay; record its barcode on success.
 * `log_skip` logs why a disk was not placed; a "change" probe passes 0, as
 * most changes of a disk that is not a cartridge are of no interest here,
 * and then only -v logs it.
 *
 * A cartridge the library already holds is taken over when this bay's disk
 * carries its barcode: one placed before the monitor (re)started, by an
 * earlier monitor or by mhvtl-raw-provision. Without that, pulling the disk
 * would leave the cartridge in the library with nothing behind it. A barcode
 * another bay placed is a duplicate disk and is left alone. */
static void handle_add(struct bay_mon *mons, size_t n, struct bay_mon *mon,
		       int verbose_log, int log_skip)
{
	struct raw_bay_result res;
	struct mhvtl_raw_probe pr;
	const struct bay_mon *owner;

	if (raw_probe(mon->bay->by_path, &pr) == 0 &&
	    (owner = owner_of(mons, n, mon, pr.barcode)) != NULL) {
		MHVTL_LOG("bay %s not placed: %s is already placed from bay %s",
			  mon->bay->by_path, pr.barcode, owner->bay->by_path);
		return;
	}

	if (raw_bay_place_one(mon->bay, &res, verbose_log) == 0 && res.placed) {
		snprintf(mon->placed_barcode, sizeof(mon->placed_barcode),
			 "%s", res.barcode);
		mon->has_placed = 1;
		MHVTL_LOG("placed %s (library %d MAP, bay %s)",
			  res.barcode, mon->bay->library_id, mon->bay->by_path);
	} else if (res.already_present) {
		snprintf(mon->placed_barcode, sizeof(mon->placed_barcode),
			 "%s", res.barcode);
		mon->has_placed = 1;
		MHVTL_LOG("%s already in library %d, tracking it for bay %s",
			  res.barcode, mon->bay->library_id, mon->bay->by_path);
	} else {
		/* Non-MVTL / unreadable / library refusal: a skip, not an
		 * error. Report the reason and leave the bay unplaced. */
		if (log_skip || verbose_log)
			MHVTL_LOG("bay %s not placed: %s", mon->bay->by_path,
				  res.skip_reason ? res.skip_reason
						  : "not provisioned");
	}
}

/* Remove the cartridge we placed from this bay (device is gone; use stored
 * barcode — it can no longer be probed). */
static void handle_remove(struct bay_mon *mon, int verbose_log)
{
	if (!mon->has_placed)
		return;

	if (raw_bay_remove(mon->bay, mon->placed_barcode, verbose_log) == 0)
		MHVTL_LOG("removed %s (library %d MAP, bay %s)",
			  mon->placed_barcode, mon->bay->library_id,
			  mon->bay->by_path);
	else
		MHVTL_ERR("bay %s: library did not confirm removal of %s",
			  mon->bay->by_path, mon->placed_barcode);

	mon->placed_barcode[0] = '\0';
	mon->has_placed = 0;
}

/*
 * Coldplug: a disk already sitting in a bay when the daemon starts never emits
 * an "add" event, so enumerate the present block disks once and place any that
 * match a bay. Enumerated AFTER the monitor is enabled so an insertion racing
 * startup is caught by the event loop rather than lost (a disk seen by both the
 * enumerate and an event is simply placed twice; the library rejects the second
 * "load map" as already-present, which raw_bay_place_one() reports as a skip).
 */
static void coldplug(struct udev *udev, struct bay_mon *mons, size_t n,
		     int verbose_log)
{
	struct udev_enumerate	*en;
	struct udev_list_entry	*list, *entry;

	en = udev_enumerate_new(udev);
	if (!en) {
		MHVTL_ERR("coldplug: udev_enumerate_new failed");
		return;
	}
	udev_enumerate_add_match_subsystem(en, "block");
	udev_enumerate_scan_devices(en);
	list = udev_enumerate_get_list_entry(en);

	udev_list_entry_foreach(entry, list) {
		const char		*syspath = udev_list_entry_get_name(entry);
		struct udev_device	*dev;
		const char		*devtype, *id_path, *devnode;
		int			 idx;

		dev = udev_device_new_from_syspath(udev, syspath);
		if (!dev)
			continue;

		devtype = udev_device_get_devtype(dev);
		if (!devtype || strcmp(devtype, "disk") != 0) {
			udev_device_unref(dev);	/* ignore partitions */
			continue;
		}

		id_path = udev_device_get_property_value(dev, "ID_PATH");
		devnode = udev_device_get_devnode(dev);
		idx = match_bay(mons, n, id_path, devnode);
		if (idx >= 0)
			handle_add(mons, n, &mons[idx], verbose_log, 1);

		udev_device_unref(dev);
	}

	udev_enumerate_unref(en);
}

/* Drain and act on every device currently queued on the monitor fd. */
static void drain_events(struct udev_monitor *mon, struct bay_mon *mons,
			 size_t n, int verbose_log)
{
	struct udev_device *dev;

	while ((dev = udev_monitor_receive_device(mon)) != NULL) {
		const char	*devtype = udev_device_get_devtype(dev);
		const char	*action  = udev_device_get_action(dev);
		const char	*id_path, *devnode;
		int		 idx;

		if (!devtype || strcmp(devtype, "disk") != 0 || !action) {
			udev_device_unref(dev);	/* ignore partitions / noise */
			continue;
		}

		id_path = udev_device_get_property_value(dev, "ID_PATH");
		devnode = udev_device_get_devnode(dev);
		idx = match_bay(mons, n, id_path, devnode);
		if (idx < 0) {
			udev_device_unref(dev);
			continue;
		}

		if (strcmp(action, "add") == 0)
			handle_add(mons, n, &mons[idx], verbose_log, 1);
		else if (strcmp(action, "remove") == 0)
			handle_remove(&mons[idx], verbose_log);
		else if (strcmp(action, "change") == 0 && !mons[idx].has_placed)
			/* A placed cartridge changes every time the drive
			 * closes it; only an empty bay is worth a probe. */
			handle_add(mons, n, &mons[idx], verbose_log, 0);
		/* everything else: ignore. */

		udev_device_unref(dev);
	}
}

int main(int argc, char **argv)
{
	const char	*config = NULL;
	char		 default_config[1024];
	char		 dc[1024];
	struct raw_bay	*bays = NULL;
	size_t		 count = 0;
	struct bay_mon	*mons = NULL;
	char		 errbuf[256];
	int		 opt_verbose = 0;
	struct udev	*udev = NULL;
	struct udev_monitor *mon = NULL;
	struct pollfd	 pfd;
	int		 fd, rc = 0;

	static struct option opts[] = {
		{ "config",     required_argument, NULL, 'c' },
		{ "foreground", no_argument,       NULL, 'f' },
		{ "verbose",    no_argument,       NULL, 'v' },
		{ "help",       no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int c;

	/* Foreground tool: route MHVTL_LOG/MHVTL_ERR to the console. */
	extern uint8_t debug, verbose;
	debug = 1;

	while ((c = getopt_long(argc, argv, "c:fvh", opts, NULL)) != -1) {
		switch (c) {
		case 'c': config = optarg; break;
		case 'f': break;		/* foreground is already the default */
		case 'v': opt_verbose = 1; break;
		case 'h': usage(stdout); return 0;
		default:  usage(stderr); return 2;
		}
	}
	verbose = opt_verbose;

	if (!config) {
		snprintf(default_config, sizeof(default_config),
			 "%s/mhvtl_raw_bays.conf", raw_bay_config_dir());
		config = default_config;
	}

	/*
	 * Pre-check device.conf BEFORE anything can call into raw_bay_ops: the
	 * underlying find_media_home_directory() exit()s on an unreadable
	 * device.conf, which must never happen from inside the running event
	 * loop. raw_bay_config_dir() is the same resolver that function uses.
	 */
	snprintf(dc, sizeof(dc), "%s/device.conf", raw_bay_config_dir());
	if (access(dc, R_OK) != 0) {
		MHVTL_ERR("cannot read %s: %s — refusing to start", dc,
			  strerror(errno));
		return 1;
	}

	if (raw_bay_conf_parse(config, &bays, &count, errbuf, sizeof(errbuf)) < 0) {
		MHVTL_ERR("%s", errbuf);
		return 1;
	}
	if (count == 0) {
		MHVTL_LOG("no raw-disk bays configured in %s — nothing to monitor",
			  config);
		raw_bay_conf_free(bays);
		return 0;
	}

	mons = calloc(count, sizeof(*mons));
	if (!mons) {
		MHVTL_ERR("out of memory");
		raw_bay_conf_free(bays);
		return 1;
	}

	/* Build the match table: ID_PATH suffix for by-path bays, resolved
	 * devnode (coldplug only) for any other binding. */
	for (size_t i = 0; i < count; i++) {
		mons[i].bay = &bays[i];
		if (strncmp(bays[i].by_path, BY_PATH_PREFIX,
			    strlen(BY_PATH_PREFIX)) == 0) {
			mons[i].match_key =
				bays[i].by_path + strlen(BY_PATH_PREFIX);
		} else {
			mons[i].match_key = NULL;
			if (!realpath(bays[i].by_path, mons[i].resolved))
				mons[i].resolved[0] = '\0';
			MHVTL_LOG("bay %s is not under %s: coldplug-only, no "
				  "hot-remove tracking (by-path is the supported "
				  "binding)", bays[i].by_path, BY_PATH_PREFIX);
		}
	}

	udev = udev_new();
	if (!udev) {
		MHVTL_ERR("udev_new failed");
		rc = 1;
		goto out;
	}

	mon = udev_monitor_new_from_netlink(udev, "udev");
	if (!mon) {
		MHVTL_ERR("udev_monitor_new_from_netlink failed");
		rc = 1;
		goto out;
	}
	udev_monitor_filter_add_match_subsystem_devtype(mon, "block", "disk");
	if (udev_monitor_enable_receiving(mon) < 0) {
		MHVTL_ERR("udev_monitor_enable_receiving failed");
		rc = 1;
		goto out;
	}
	fd = udev_monitor_get_fd(mon);
	if (fd < 0) {
		MHVTL_ERR("udev_monitor_get_fd failed");
		rc = 1;
		goto out;
	}

	/* Enumerate present disks AFTER enabling the monitor (see coldplug()). */
	coldplug(udev, mons, count, opt_verbose);

	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = caught_signal;
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
	}

	MHVTL_LOG("monitoring %zu bay(s) for raw-disk hot-plug events", count);

	pfd.fd = fd;
	pfd.events = POLLIN;
	while (!stop_requested) {
		int n = poll(&pfd, 1, -1);

		if (n < 0) {
			if (errno == EINTR)
				continue;	/* signal: re-check stop flag */
			MHVTL_ERR("poll failed: %s", strerror(errno));
			rc = 1;
			break;
		}
		if (pfd.revents & POLLIN)
			drain_events(mon, mons, count, opt_verbose);
	}

	MHVTL_LOG("shutting down");

out:
	if (mon)
		udev_monitor_unref(mon);
	if (udev)
		udev_unref(udev);
	free(mons);
	raw_bay_conf_free(bays);
	return rc;
}
