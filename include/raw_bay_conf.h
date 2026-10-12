/*
 * Raw-disk bay map configuration (Phase 2, Stage A).
 *
 * Binds a library Import/Export (MAP) slot to a physical disk bay identified by
 * a stable /dev/disk/by-path node. The bay is the physical anchor; the cartridge
 * barcode is discovered from the MVTL superblock at provisioning time (design
 * docs/design/raw-disk-backend.md §2). Parsed by the static provisioner
 * (mhvtl-raw-provision) and, later, the libudev hot-plug monitor.
 */
#ifndef _RAW_BAY_CONF_H_
#define _RAW_BAY_CONF_H_

#include <stddef.h>

#define RAW_BAY_PATH_MAX	1024	/* device node path */
#define RAW_BAY_ID_MAX		256	/* optional audit id  */

struct raw_bay {
	int  library_id;		/* mhvtl library instance id (>= 0)   */
	int  map_slot;			/* 1-based MAP/IE slot index (>= 1)   */
	char by_path[RAW_BAY_PATH_MAX];	/* the binding: stable device node    */
	char by_id[RAW_BAY_ID_MAX];	/* audit identity only, "" if absent  */
};

/*
 * Parse the bay config at `path` into a newly-allocated array returned through
 * *out / *count. '#' comment lines and blank lines are ignored. Each data line
 * is: "<library_id> <map_slot> <by-path> [by-id=<identifier>]". The by-path is
 * the binding; by-id, if present, is recorded for audit only.
 *
 * Returns 0 on success (caller frees *out with raw_bay_conf_free()). An empty or
 * all-comment file is success with *count == 0 and *out == NULL. Returns -1 on
 * error with a human-readable message (naming the offending line number) in
 * errbuf; on error *out is NULL and *count is 0.
 */
int raw_bay_conf_parse(const char *path, struct raw_bay **out, size_t *count,
		       char *errbuf, size_t errlen);

/* Release an array returned by raw_bay_conf_parse(). NULL is a no-op. */
void raw_bay_conf_free(struct raw_bay *bays);

#endif /* _RAW_BAY_CONF_H_ */
