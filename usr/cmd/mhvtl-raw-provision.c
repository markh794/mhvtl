/*
 * mhvtl-raw-provision — static raw-disk bay provisioner (Phase 2, Stage A).
 *
 * A disk bay is a library Import/Export (MAP) slot bound to a stable device
 * node by the bay config (see etc/mhvtl_raw_bays.conf.sample). For each bay
 * that currently holds a formatted raw-disk cartridge, this tool:
 *
 *   1. probes the device for a valid MVTL superblock and reads its barcode
 *      (raw_probe);
 *   2. creates (or refreshes) a home_directory/<barcode> symlink to the STABLE
 *      by-path node, so load_tape() later resolves <barcode> -> S_ISBLK -> raw
 *      backend (design docs/design/raw-disk-backend.md §2);
 *   3. asks the library to place the cartridge into an empty MAP slot by sending
 *      "load map <barcode>" over the SysV message queue (same addressing as
 *      vtlcmd: receiver message-type == the library instance id).
 *
 * Two present bays whose disks report the same barcode are a collision: neither
 * is loaded. Absent/unreadable/unformatted bays are skipped, not errors.
 *
 * Usage:
 *   mhvtl-raw-provision [--config <path>] [--verbose] [-h|--help]
 *
 * Default config: $MHVTL_CONFIG_PATH/mhvtl_raw_bays.conf (else /etc/mhvtl/...).
 * Exit status is 0 whenever the run completed (even if some bays were skipped);
 * non-zero only on a hard error such as a config parse failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#include "raw_bay_conf.h"
#include "cart_backend_raw.h"
#include "vtllib.h"
#include "q.h"

/* Every mhvtl program that links libvtlscsi defines this (used by logging). */
char mhvtl_driver_name[] = "mhvtl-raw-provision";

static int opt_verbose;

/* Per-bay outcome, parallel to the parsed bay array. */
struct bay_state {
	int		 valid;		/* a readable MVTL cartridge is present   */
	int		 placed;	/* library confirmed the cartridge placed */
	char		 barcode[33];	/* probed barcode (valid bays only)       */
	const char	*skip_reason;	/* why the bay was skipped, else NULL     */
	char		 reply_msg[64];	/* storage for a library rejection reply  */
};

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: mhvtl-raw-provision [--config <path>] [--verbose]\n"
		"\n"
		"  --config <path>  bay map config (default: "
		"$MHVTL_CONFIG_PATH/mhvtl_raw_bays.conf)\n"
		"  --verbose        log per-bay probe decisions to stderr\n"
		"  -h, --help       show this help\n");
}

/*
 * Create or refresh home_directory/<barcode> -> by_path. Returns 0 if the link
 * is in place afterwards, -1 (with *reason set) if a conflicting entry exists or
 * the symlink could not be created.
 */
static int ensure_symlink(const char *barcode, const char *by_path,
			  const char **reason)
{
	char		link[HOME_DIR_PATH_SZ + 1 + 33];
	struct stat	st;
	char		cur[RAW_BAY_PATH_MAX];
	ssize_t		n;

	snprintf(link, sizeof(link), "%s/%s", home_directory, barcode);

	if (lstat(link, &st) == 0) {
		if (!S_ISLNK(st.st_mode)) {
			*reason = "a non-symlink file already occupies "
				  "home_directory/<barcode>";
			return -1;
		}
		n = readlink(link, cur, sizeof(cur) - 1);
		if (n >= 0) {
			cur[n] = '\0';
			if (strcmp(cur, by_path) == 0)
				return 0;	/* already correct, leave it */
		}
		/* Stale/incorrect symlink: refresh it to the current binding. */
		if (unlink(link) < 0) {
			*reason = "cannot replace stale home_directory/<barcode> symlink";
			return -1;
		}
	} else if (errno != ENOENT) {
		*reason = "cannot stat home_directory/<barcode>";
		return -1;
	}

	if (symlink(by_path, link) < 0) {
		*reason = "cannot create home_directory/<barcode> symlink";
		return -1;
	}
	return 0;
}

/*
 * Send a control command to library `library_id` and wait for its reply.
 * Mirrors vtlcmd: receiver message-type == the library id, reply comes back on
 * VTLCMD_Q (my_id). Returns 0 and fills `reply` on a reply; -1 if send failed or
 * no reply arrived within the window (e.g. no library instance is listening).
 */
static int lib_cmd(long library_id, const char *cmd, char *reply, size_t rlen)
{
	int		qid;
	struct q_entry	r;
	char		buf[MAXTEXTLEN + 1];

	if (reply && rlen)
		reply[0] = '\0';

	snprintf(buf, sizeof(buf), "%s", cmd);
	if (send_msg(buf, library_id) < 0)
		return -1;

	qid = init_queue();
	if (qid < 0)
		return -1;

	/* The library replies on the VTLCMD_Q message-type. Poll briefly; a
	 * missing reply means no library is listening on this id. */
	for (int attempt = 0; attempt < 80; attempt++) {
		if (msgrcv(qid, &r, MAXOBN, VTLCMD_Q, IPC_NOWAIT) >= 0) {
			if (reply && rlen)
				snprintf(reply, rlen, "%s", r.msg.text);
			if (opt_verbose)
				fprintf(stderr, "library %ld reply to '%s': %s\n",
					library_id, cmd, r.msg.text);
			return 0;
		}
		usleep(25000);	/* 25ms, up to 2s total */
	}
	return -1;
}

/* A library "list map"/"load map" OK reply is the literal token "OK". */
static int reply_ok(const char *reply)
{
	return reply && strcmp(reply, "OK") == 0;
}

int main(int argc, char **argv)
{
	const char	*config = NULL;
	const char	*cfg_dir;
	char		 default_config[1024];
	struct raw_bay	*bays = NULL;
	struct bay_state *state = NULL;
	size_t		 count = 0;
	char		 errbuf[256];

	static struct option opts[] = {
		{ "config",  required_argument, NULL, 'c' },
		{ "verbose", no_argument,       NULL, 'v' },
		{ "help",    no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int c;

	while ((c = getopt_long(argc, argv, "c:vh", opts, NULL)) != -1) {
		switch (c) {
		case 'c': config = optarg; break;
		case 'v': opt_verbose = 1; break;
		case 'h': usage(stdout); return 0;
		default:  usage(stderr); return 2;
		}
	}

	cfg_dir = getenv("MHVTL_CONFIG_PATH");
	if (!cfg_dir || !cfg_dir[0])
		cfg_dir = MHVTL_CONFIG_PATH;

	if (!config) {
		snprintf(default_config, sizeof(default_config),
			 "%s/mhvtl_raw_bays.conf", cfg_dir);
		config = default_config;
	}

	/* send_msg() sets snd_id = my_id; VTLCMD_Q makes library replies come
	 * back on the VTLCMD_Q message-type, exactly like vtlcmd. */
	my_id = VTLCMD_Q;

	if (raw_bay_conf_parse(config, &bays, &count, errbuf, sizeof(errbuf)) < 0) {
		fprintf(stderr, "mhvtl-raw-provision: %s\n", errbuf);
		return 1;
	}

	if (count == 0) {
		printf("No raw-disk bays configured in %s\n", config);
		raw_bay_conf_free(bays);
		return 0;
	}

	state = calloc(count, sizeof(*state));
	if (!state) {
		fprintf(stderr, "mhvtl-raw-provision: out of memory\n");
		raw_bay_conf_free(bays);
		return 1;
	}

	/* Pass 1: probe every bay; record the barcode of valid cartridges. */
	for (size_t i = 0; i < count; i++) {
		struct mhvtl_raw_probe pr;

		if (raw_probe(bays[i].by_path, &pr) == 0) {
			state[i].valid = 1;
			snprintf(state[i].barcode, sizeof(state[i].barcode),
				 "%s", pr.barcode);
			if (opt_verbose)
				fprintf(stderr,
					"bay %s: MVTL cartridge '%s' (library %d MAP %d)\n",
					bays[i].by_path, state[i].barcode,
					bays[i].library_id, bays[i].map_slot);
		} else {
			state[i].skip_reason =
				"no readable MVTL cartridge in bay";
			if (opt_verbose)
				fprintf(stderr,
					"bay %s: skipped (%s: %s)\n",
					bays[i].by_path, state[i].skip_reason,
					strerror(errno));
		}
	}

	/* Pass 2: dedup. A barcode present in more than one bay is a collision;
	 * load none of them so a cartridge can never be bound to two bays. */
	for (size_t i = 0; i < count; i++) {
		if (!state[i].valid)
			continue;
		for (size_t j = i + 1; j < count; j++) {
			if (!state[j].valid)
				continue;
			if (strcmp(state[i].barcode, state[j].barcode) != 0)
				continue;
			state[i].valid = 0;
			state[j].valid = 0;
			state[i].skip_reason =
				"duplicate barcode in multiple bays (collision)";
			state[j].skip_reason = state[i].skip_reason;
			if (opt_verbose)
				fprintf(stderr,
					"collision: barcode '%s' in bays %s and %s; "
					"loading neither\n",
					state[i].barcode, bays[i].by_path,
					bays[j].by_path);
		}
	}

	/* Is there anything left to place after probing + dedup? */
	int any_valid = 0;
	for (size_t i = 0; i < count; i++)
		if (state[i].valid) {
			any_valid = 1;
			break;
		}

	/*
	 * Resolving a library's home directory reads <cfg_dir>/device.conf, and the
	 * underlying helper exits the process on an unreadable device.conf. Only
	 * required when we actually have a cartridge to place; check it up front and
	 * fail cleanly, rather than aborting mid-run after some cartridges were sent.
	 */
	if (any_valid) {
		char dc[1024];

		snprintf(dc, sizeof(dc), "%s/device.conf", cfg_dir);
		if (access(dc, R_OK) != 0) {
			fprintf(stderr, "mhvtl-raw-provision: cannot read %s: %s\n",
				dc, strerror(errno));
			free(state);
			raw_bay_conf_free(bays);
			return 1;
		}
	}

	/*
	 * Pass 3a: open the MAP of each library that has a cartridge to place. The
	 * library starts with the MAP closed and rejects "load map" until it is
	 * opened, so a self-contained provisioner opens it here (and closes it in
	 * pass 3c to restore the default). A library whose MAP will not open (no
	 * listener / error) has its bays skipped, never falsely reported placed.
	 */
	int	*lib_ids = calloc(count, sizeof(*lib_ids));
	int	*lib_open = calloc(count, sizeof(*lib_open));
	size_t	 nlib = 0;

	if (!lib_ids || !lib_open) {
		fprintf(stderr, "mhvtl-raw-provision: out of memory\n");
		free(lib_ids);
		free(lib_open);
		free(state);
		raw_bay_conf_free(bays);
		return 1;
	}

	for (size_t i = 0; i < count; i++) {
		size_t k;
		char reply[MAXTEXTLEN + 1];

		if (!state[i].valid)
			continue;
		for (k = 0; k < nlib; k++)
			if (lib_ids[k] == bays[i].library_id)
				break;
		if (k < nlib)
			continue;	/* library already opened (or tried) */
		lib_ids[nlib] = bays[i].library_id;
		lib_open[nlib] = (lib_cmd(bays[i].library_id, "open map",
					  reply, sizeof(reply)) == 0 &&
				  reply_ok(reply));
		nlib++;
	}

	/*
	 * Pass 3b: per unique valid bay, resolve the home dir, refresh the symlink,
	 * and load the cartridge into the MAP -- marking it placed only when the
	 * library confirms with "OK".
	 */
	for (size_t i = 0; i < count; i++) {
		const char *reason = NULL;
		char cmd[MAXTEXTLEN + 1], reply[MAXTEXTLEN + 1];
		size_t k;

		if (!state[i].valid)
			continue;

		for (k = 0; k < nlib; k++)
			if (lib_ids[k] == bays[i].library_id)
				break;
		if (k == nlib || !lib_open[k]) {
			state[i].skip_reason =
				"library not responding or MAP could not be opened";
			continue;
		}

		/* Resolve home_directory for this bay's library from device.conf
		 * (NULL -> use $MHVTL_CONFIG_PATH, like every other mhvtl tool). */
		find_media_home_directory(NULL, bays[i].library_id);
		if (home_directory[0] == '\0') {
			state[i].skip_reason =
				"library has no home directory in device.conf";
			continue;
		}

		if (ensure_symlink(state[i].barcode, bays[i].by_path,
				   &reason) < 0) {
			state[i].skip_reason = reason;
			if (opt_verbose)
				fprintf(stderr, "bay %s: %s\n",
					bays[i].by_path, reason);
			continue;
		}

		snprintf(cmd, sizeof(cmd), "load map %s", state[i].barcode);
		if (lib_cmd(bays[i].library_id, cmd, reply, sizeof(reply)) != 0) {
			state[i].skip_reason =
				"no reply from library for 'load map'";
			continue;
		}
		if (!reply_ok(reply)) {
			/* e.g. "MAP not opened", "MAP Full", "barcode already in
			 * library", "barcode length too long". Report it verbatim. */
			snprintf(state[i].reply_msg, sizeof(state[i].reply_msg),
				 "load map rejected: %.40s", reply);
			state[i].skip_reason = state[i].reply_msg;
			continue;
		}
		state[i].placed = 1;
	}

	/* Pass 3c: close the MAP of every library we opened (restore default). */
	for (size_t k = 0; k < nlib; k++) {
		char reply[MAXTEXTLEN + 1];

		if (lib_open[k])
			lib_cmd(lib_ids[k], "close map", reply, sizeof(reply));
	}
	free(lib_ids);
	free(lib_open);

	/* Summary. */
	for (size_t i = 0; i < count; i++) {
		if (state[i].placed)
			printf("PLACED %s in library %d MAP\n",
			       state[i].barcode, bays[i].library_id);
		else
			printf("SKIPPED %s: %s\n", bays[i].by_path,
			       state[i].skip_reason ? state[i].skip_reason
						    : "not provisioned");
	}

	free(state);
	raw_bay_conf_free(bays);
	return 0;
}
