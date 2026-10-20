/*
 * raw_bay_ops — shared raw-disk bay operations (Phase 2).
 *
 * A disk bay is a library Import/Export (MAP) slot bound to a stable device
 * node by the bay config (struct raw_bay, include/raw_bay_conf.h). Placing a
 * bay means: probe the disk for a formatted MVTL cartridge, create the
 * home_directory/<barcode> symlink that lets load_tape() resolve <barcode> ->
 * S_ISBLK -> raw backend, and load the cartridge into an empty MAP slot of the
 * bay's library over the SysV control queue.
 *
 * This logic was first written inline in the Stage-A static provisioner
 * (usr/cmd/mhvtl-raw-provision.c). It is factored out here, compiled INTO
 * libvtlscsi, so the Stage-B libudev hot-plug daemon (US-B2) can drive the same
 * place/remove operations on a single bay in response to insertion/removal
 * events, without duplicating the probe/symlink/load-map logic.
 */
#ifndef _RAW_BAY_OPS_H_
#define _RAW_BAY_OPS_H_

#include <stddef.h>
#include "raw_bay_conf.h"

/*
 * Per-bay placement outcome, parallel to the parsed bay array passed to
 * raw_bay_place_all(). On a successful placement `placed` is 1 and `barcode`
 * holds the probed barcode; otherwise `skip_reason` explains why the bay was
 * not placed (NULL means "not provisioned").
 */
struct raw_bay_result {
	int		 placed;	/* library confirmed the cartridge placed */
	char		 barcode[33];	/* probed barcode (placed/valid bays)     */
	const char	*skip_reason;	/* why the bay was skipped, else NULL     */
	char		 reply_msg[64];	/* storage for a library rejection reply  */
};

/*
 * Resolve the single mhvtl config directory: $MHVTL_CONFIG_PATH if set and
 * non-empty, else the compiled-in MHVTL_CONFIG_PATH default. Routing every
 * caller (the device.conf pre-check AND find_media_home_directory()) through
 * this one helper keeps them pointed at the SAME directory.
 *
 * NOTE: get_config() may still be further redirected by an MHVTL_CONFIG_PATH
 * key inside <dir>/mhvtl.conf; this helper does not read that file. Resolving
 * via a single helper is the fix point — a future change that must honour the
 * mhvtl.conf key should extend this one function.
 */
const char *raw_bay_config_dir(void);

/*
 * Send control command `cmd` to library `library_id` and wait for its reply.
 * Mirrors vtlcmd addressing: the command is sent with rcv_id == library_id and
 * snd_id == mhvtl_reply_id(), and the reply comes back on that message-type,
 * which no other client (vtlcmd, mhvtld) reads.
 *
 * HARDENED for repeated use by a long-running daemon: before sending, any stale
 * replies already sitting on our reply message-type are DRAINED, so a reply
 * left over from an earlier command can never be mis-read as this command's
 * answer. Returns 0 and fills `reply` on a reply; -1 if the send failed or no
 * reply arrived within the bounded window (e.g. no library is listening).
 *
 * Sets my_id = mhvtl_reply_id() so replies route back here regardless of prior
 * state.
 */
int raw_bay_lib_cmd(long library_id, const char *cmd, char *reply, size_t rlen,
		    int verbose_log);

/* A library "load map"/"open map"/... OK reply is the literal token "OK". */
int raw_bay_reply_ok(const char *reply);

/*
 * Place a single bay. Probes bay->by_path for a valid MVTL cartridge; if one is
 * present, resolves home_directory for bay->library_id, refreshes the
 * home_directory/<barcode> symlink to bay->by_path, then opens the MAP, loads
 * the cartridge ("load map <barcode>"), and closes the MAP again — marking
 * res->placed only when the library confirms with "OK". Fills res->barcode and,
 * on any non-placement, res->skip_reason. Returns 0 if the bay was placed, -1
 * otherwise (res->skip_reason set).
 *
 * NOTE: resolving the home directory reads <config_dir>/device.conf, and the
 * underlying find_media_home_directory() aborts the process if that file is
 * unreadable. Standalone callers (hot-plug daemon) should pre-check it exists
 * with raw_bay_config_dir(); raw_bay_place_all() does this pre-check for you.
 */
int raw_bay_place_one(const struct raw_bay *bay, struct raw_bay_result *res,
		      int verbose_log);

/*
 * Place an array of `count` bays into `results` (caller-allocated, `count`
 * entries). Applies the same cross-bay duplicate-barcode safety as the Stage-A
 * provisioner: a barcode present on more than one present bay is a collision
 * and NONE of the colliding bays are loaded. Returns 0 when the run completed
 * (inspect each result); -1 on a hard error (e.g. device.conf unreadable) with
 * a human-readable message in errbuf and no placements attempted.
 */
int raw_bay_place_all(const struct raw_bay *bays, size_t count,
		      struct raw_bay_result *results, int verbose_log,
		      char *errbuf, size_t errlen);

/*
 * Remove a cartridge previously placed into a bay's library MAP. If `barcode`
 * is non-NULL and non-empty it names the cartridge directly; otherwise the disk
 * at bay->by_path is probed for its barcode. Drops the home_directory/<barcode>
 * symlink and sends "empty map <barcode>" to bay->library_id so the MAP slot
 * holding it is cleared; other MAP slots are left alone. (If the cartridge is
 * currently loaded in a drive, the library handles media-not-present itself —
 * this never force-unloads a drive.) Safe no-op if no barcode can be
 * determined and nothing is linked. Returns 0 on success/no-op, 1 if the
 * cartridge was not in the MAP (moved to a storage slot or a drive, which is
 * left alone, or emptied already), -1 if the library did not confirm the MAP
 * was cleared.
 */
int raw_bay_remove(const struct raw_bay *bay, const char *barcode, int verbose_log);

#endif /* _RAW_BAY_OPS_H_ */
