/*
 * raw_bay_ops — shared raw-disk bay operations (Phase 2).
 *
 * Factored out of the Stage-A static provisioner (usr/cmd/mhvtl-raw-provision.c)
 * so the Stage-B libudev hot-plug daemon (US-B2) can drive the same bay
 * place/remove operations. See include/raw_bay_ops.h for the interface.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#include "raw_bay_ops.h"
#include "raw_bay_conf.h"
#include "cart_backend_raw.h"
#include "vtllib.h"
#include "q.h"

const char *raw_bay_config_dir(void)
{
	const char *dir = getenv("MHVTL_CONFIG_PATH");

	if (!dir || !dir[0])
		dir = MHVTL_CONFIG_PATH;
	return dir;
}

int raw_bay_reply_ok(const char *reply)
{
	return reply && strcmp(reply, "OK") == 0;
}

int raw_bay_lib_cmd(long library_id, const char *cmd, char *reply, size_t rlen,
		    int verbose_log)
{
	int		qid;
	struct q_entry	r;
	char		buf[MAXTEXTLEN + 1];

	if (reply && rlen)
		reply[0] = '\0';

	/* send_msg() uses my_id as snd_id; VTLCMD_Q makes the library's reply
	 * come back on the VTLCMD_Q message-type, exactly like vtlcmd. */
	my_id = VTLCMD_Q;

	qid = init_queue();
	if (qid < 0)
		return -1;

	/*
	 * Drain-before-send: clear any stale replies already queued on the
	 * VTLCMD_Q message-type before issuing this command. A reply left over
	 * from a previous command (or a previous run) would otherwise be read
	 * back as THIS command's answer. Draining first means the next VTLCMD_Q
	 * message we read after sending is attributable to this send. This is
	 * the hardening that makes the helper safe for a long-running daemon
	 * that issues many commands over the one shared queue.
	 */
	while (msgrcv(qid, &r, MAXOBN, VTLCMD_Q, IPC_NOWAIT) >= 0) {
		if (verbose_log)
			fprintf(stderr,
				"raw_bay: drained stale reply on VTLCMD_Q: %s\n",
				r.msg.text);
	}

	snprintf(buf, sizeof(buf), "%s", cmd);
	if (send_msg(buf, library_id) < 0)
		return -1;

	/* Poll briefly for the reply; a missing reply means no library is
	 * listening on this id. 80 * 25ms = up to 2s. */
	for (int attempt = 0; attempt < 80; attempt++) {
		if (msgrcv(qid, &r, MAXOBN, VTLCMD_Q, IPC_NOWAIT) >= 0) {
			if (reply && rlen)
				snprintf(reply, rlen, "%s", r.msg.text);
			if (verbose_log)
				fprintf(stderr, "library %ld reply to '%s': %s\n",
					library_id, cmd, r.msg.text);
			return 0;
		}
		usleep(25000);
	}
	return -1;
}

/*
 * Create or refresh home_directory/<barcode> -> by_path. Returns 0 if the link
 * is in place afterwards, -1 (with *reason set) if a conflicting entry exists or
 * the symlink could not be created. Semantics preserved from the Stage-A
 * provisioner: keep a correct existing link, refresh a stale one, refuse a
 * non-symlink conflict.
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
 * Place a bay whose barcode has already been probed into res->barcode: resolve
 * the home directory, refresh the symlink, then open/load/close the MAP. Shared
 * by raw_bay_place_one() (which probes first) and raw_bay_place_all() (which
 * probes every bay once up front for the dedup pass), so the probe/symlink/
 * load-map logic lives in exactly one place. Returns 0 if placed, -1 otherwise
 * (res->skip_reason set).
 */
static int place_resolved(const struct raw_bay *bay, struct raw_bay_result *res,
			  int verbose_log)
{
	const char	*reason = NULL;
	char		 cmd[MAXTEXTLEN + 1], reply[MAXTEXTLEN + 1];
	int		 map_opened = 0;

	res->placed = 0;
	res->reply_msg[0] = '\0';

	/* Resolve home_directory for this bay's library from device.conf
	 * (NULL -> use $MHVTL_CONFIG_PATH, like every other mhvtl tool). */
	find_media_home_directory(NULL, bay->library_id);
	if (home_directory[0] == '\0') {
		res->skip_reason = "library has no home directory in device.conf";
		return -1;
	}

	if (ensure_symlink(res->barcode, bay->by_path, &reason) < 0) {
		res->skip_reason = reason;
		if (verbose_log)
			fprintf(stderr, "bay %s: %s\n", bay->by_path, reason);
		return -1;
	}

	/* The library starts with the MAP closed and rejects "load map" until it
	 * is opened, so open it here and close it again afterwards to restore the
	 * default. A library that will not open the MAP (no listener / error) has
	 * its bay skipped, never falsely reported placed. */
	if (raw_bay_lib_cmd(bay->library_id, "open map", reply, sizeof(reply),
			    verbose_log) != 0 || !raw_bay_reply_ok(reply)) {
		res->skip_reason =
			"library not responding or MAP could not be opened";
		return -1;
	}
	map_opened = 1;

	snprintf(cmd, sizeof(cmd), "load map %s", res->barcode);
	if (raw_bay_lib_cmd(bay->library_id, cmd, reply, sizeof(reply),
			    verbose_log) != 0) {
		res->skip_reason = "no reply from library for 'load map'";
	} else if (!raw_bay_reply_ok(reply)) {
		/* e.g. "MAP not opened", "MAP Full", "barcode already in
		 * library", "barcode length too long". Report it verbatim. */
		snprintf(res->reply_msg, sizeof(res->reply_msg),
			 "load map rejected: %.40s", reply);
		res->skip_reason = res->reply_msg;
	} else {
		res->placed = 1;
	}

	if (map_opened)
		raw_bay_lib_cmd(bay->library_id, "close map", reply,
				sizeof(reply), verbose_log);

	return res->placed ? 0 : -1;
}

int raw_bay_place_one(const struct raw_bay *bay, struct raw_bay_result *res,
		      int verbose_log)
{
	struct mhvtl_raw_probe pr;

	res->placed = 0;
	res->barcode[0] = '\0';
	res->skip_reason = NULL;
	res->reply_msg[0] = '\0';

	if (raw_probe(bay->by_path, &pr) != 0) {
		res->skip_reason = "no readable MVTL cartridge in bay";
		if (verbose_log)
			fprintf(stderr, "bay %s: skipped (%s: %s)\n",
				bay->by_path, res->skip_reason, strerror(errno));
		return -1;
	}
	snprintf(res->barcode, sizeof(res->barcode), "%s", pr.barcode);
	if (verbose_log)
		fprintf(stderr,
			"bay %s: MVTL cartridge '%s' (library %d MAP %d)\n",
			bay->by_path, res->barcode, bay->library_id,
			bay->map_slot);

	return place_resolved(bay, res, verbose_log);
}

int raw_bay_place_all(const struct raw_bay *bays, size_t count,
		      struct raw_bay_result *results, int verbose_log,
		      char *errbuf, size_t errlen)
{
	int	*valid;
	int	 any_valid = 0;

	for (size_t i = 0; i < count; i++) {
		results[i].placed = 0;
		results[i].barcode[0] = '\0';
		results[i].skip_reason = NULL;
		results[i].reply_msg[0] = '\0';
	}

	valid = calloc(count, sizeof(*valid));
	if (!valid) {
		if (errbuf && errlen)
			snprintf(errbuf, errlen, "out of memory");
		return -1;
	}

	/* Pass 1: probe every bay; record the barcode of valid cartridges. */
	for (size_t i = 0; i < count; i++) {
		struct mhvtl_raw_probe pr;

		if (raw_probe(bays[i].by_path, &pr) == 0) {
			valid[i] = 1;
			snprintf(results[i].barcode, sizeof(results[i].barcode),
				 "%s", pr.barcode);
			if (verbose_log)
				fprintf(stderr,
					"bay %s: MVTL cartridge '%s' (library %d MAP %d)\n",
					bays[i].by_path, results[i].barcode,
					bays[i].library_id, bays[i].map_slot);
		} else {
			results[i].skip_reason =
				"no readable MVTL cartridge in bay";
			if (verbose_log)
				fprintf(stderr, "bay %s: skipped (%s: %s)\n",
					bays[i].by_path, results[i].skip_reason,
					strerror(errno));
		}
	}

	/* Pass 2: dedup. A barcode present in more than one bay is a collision;
	 * load none of them so a cartridge can never be bound to two bays. */
	for (size_t i = 0; i < count; i++) {
		if (!valid[i])
			continue;
		for (size_t j = i + 1; j < count; j++) {
			if (!valid[j])
				continue;
			if (strcmp(results[i].barcode, results[j].barcode) != 0)
				continue;
			valid[i] = 0;
			valid[j] = 0;
			results[i].skip_reason =
				"duplicate barcode in multiple bays (collision)";
			results[j].skip_reason = results[i].skip_reason;
			if (verbose_log)
				fprintf(stderr,
					"collision: barcode '%s' in bays %s and %s; "
					"loading neither\n",
					results[i].barcode, bays[i].by_path,
					bays[j].by_path);
		}
	}

	for (size_t i = 0; i < count; i++)
		if (valid[i]) {
			any_valid = 1;
			break;
		}

	/*
	 * Resolving a library's home directory reads <cfg_dir>/device.conf, and
	 * find_media_home_directory() exits the process on an unreadable
	 * device.conf. Only required when we actually have a cartridge to place;
	 * check it up front and fail cleanly, rather than aborting mid-run after
	 * some cartridges were sent. raw_bay_config_dir() is the SAME resolver
	 * find_media_home_directory() uses, so both agree on one directory.
	 */
	if (any_valid) {
		char dc[1024];

		snprintf(dc, sizeof(dc), "%s/device.conf", raw_bay_config_dir());
		if (access(dc, R_OK) != 0) {
			if (errbuf && errlen)
				snprintf(errbuf, errlen, "cannot read %s: %s",
					 dc, strerror(errno));
			free(valid);
			return -1;
		}
	}

	/* Pass 3: place each surviving valid bay. The barcode was already probed
	 * in pass 1, so place via the shared helper (no redundant re-probe). */
	for (size_t i = 0; i < count; i++) {
		if (!valid[i])
			continue;
		place_resolved(&bays[i], &results[i], verbose_log);
	}

	free(valid);
	return 0;
}

int raw_bay_remove(const struct raw_bay *bay, const char *barcode, int verbose_log)
{
	struct mhvtl_raw_probe	pr;
	char			resolved[33];
	char			reply[MAXTEXTLEN + 1];
	int			map_opened = 0;
	int			rc = 0;

	/* Determine the barcode: explicit if given, else probe the disk. */
	if (barcode && barcode[0]) {
		snprintf(resolved, sizeof(resolved), "%s", barcode);
	} else if (raw_probe(bay->by_path, &pr) == 0) {
		snprintf(resolved, sizeof(resolved), "%s", pr.barcode);
	} else {
		/* Nothing identifiable in the bay and no explicit barcode:
		 * there is nothing to drop — safe no-op. */
		if (verbose_log)
			fprintf(stderr,
				"bay %s: remove is a no-op (no barcode)\n",
				bay->by_path);
		return 0;
	}

	/* Drop the home_directory/<barcode> symlink (if any). ENOENT is fine. */
	find_media_home_directory(NULL, bay->library_id);
	if (home_directory[0] != '\0') {
		char link[HOME_DIR_PATH_SZ + 1 + 33];
		struct stat st;

		snprintf(link, sizeof(link), "%s/%s", home_directory, resolved);
		if (lstat(link, &st) == 0 && S_ISLNK(st.st_mode)) {
			if (unlink(link) < 0 && verbose_log)
				fprintf(stderr,
					"bay %s: could not unlink %s: %s\n",
					bay->by_path, link, strerror(errno));
		}
	}

	/*
	 * Clear the MAP slot. "empty map" is rejected while the MAP is closed, so
	 * open it, empty it, and close it again. Emptying an already-empty MAP is
	 * a harmless no-op on the library side. If the cartridge is loaded in a
	 * drive the library handles media-not-present; we never force-unload.
	 */
	if (raw_bay_lib_cmd(bay->library_id, "open map", reply, sizeof(reply),
			    verbose_log) == 0 && raw_bay_reply_ok(reply)) {
		map_opened = 1;
		if (raw_bay_lib_cmd(bay->library_id, "empty map", reply,
				    sizeof(reply), verbose_log) != 0 ||
		    !raw_bay_reply_ok(reply))
			rc = -1;
	} else {
		rc = -1;
	}

	if (map_opened)
		raw_bay_lib_cmd(bay->library_id, "close map", reply,
				sizeof(reply), verbose_log);

	return rc;
}
