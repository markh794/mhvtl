/*
 * Raw block-device cartridge backend (Phase 2).
 *
 * One physical block device == one cartridge. This adapter implements the
 * struct cart_backend seam (include/cart_backend.h) against an mhvtl raw
 * superblock + per-partition regions (include/mhvtl_raw.h), mirroring the
 * byte-stream semantics of the file backend in usr/vtlcart.c.
 *
 * vtlcart.c selects &cart_backend_raw for a cartridge path that resolves to a
 * block device (that wiring lands in a separate increment). The device is
 * opened once per cartridge via raw_open() and released via raw_close().
 */
#ifndef _CART_BACKEND_RAW_H_
#define _CART_BACKEND_RAW_H_

#include <stdint.h>
#include "cart_backend.h"

/* The raw block-device backend instance (defined in usr/vtlcart_raw.c). */
extern struct cart_backend cart_backend_raw;

/*
 * Open a block device as the active raw cartridge: query capacity/geometry,
 * read and validate BOTH superblocks, and keep the one with the highest valid
 * generation_id. Returns 0 on success, -1 on error (errno set).
 */
int raw_open(const char *devpath);

/* Release the open device fd. No-op if no device is open. */
void raw_close(void);

/* Cartridge identity read off a device by raw_probe() (below). */
struct mhvtl_raw_probe {
	char	 barcode[33];		/* superblock barcode[32] + NUL */
	uint8_t	 cartridge_uuid[16];	/* immutable identity */
	uint32_t num_partitions;
	uint64_t generation_id;
	uint64_t total_capacity_bytes;
};

/*
 * Probe a block device for a valid MVTL cartridge WITHOUT making it the active
 * cartridge (does not touch the open-cartridge backend state). Reads+validates
 * both superblocks and reports the identity of the highest valid generation.
 * Returns 0 if a valid cartridge is present, -1 otherwise (errno set). Used by
 * the bay provisioner and hot-plug monitor to learn a disk's barcode/uuid.
 */
int raw_probe(const char *devpath, struct mhvtl_raw_probe *out);

struct MAM;

/*
 * Read the MAM of the raw cartridge on devpath into mamp (init_mam()'d by the
 * caller) WITHOUT making it the active cartridge. The device is opened
 * read-only and not O_EXCL, so this works while a drive holds the cartridge.
 * Returns 0 on success, -1 on error (errno set).
 */
int raw_read_mam(const char *devpath, struct MAM *mamp);

/*
 * Format a block device with a static multi-partition raw cartridge layout.
 *
 *   devpath         block device to format (e.g. /dev/sdX, /dev/loopN)
 *   barcode         cartridge PCL/barcode (copied into the superblock)
 *   density         media density for the MAM, e.g. "LTO8"; NULL takes it
 *                   from the barcode's media suffix (L8 = LTO-8, M8 = LTO-8
 *                   Type M, LU = LTO-4 WORM), which an 8-character barcode
 *                   must then carry. A density that contradicts the suffix
 *                   and more partitions than the media supports are refused
 *                   before the device is written
 *   partspec        comma-separated per-partition size percentages that must
 *                   total exactly 100, e.g. "92,8" or "85,5,5,5"
 *   min_record_size planning minimum record size; sizes the index region so it
 *                   can describe a data region made entirely of minimum records
 *
 * Writes the initial meta_header for each partition, a MAM describing the
 * media type and the capacity of the data regions, and both superblocks with
 * generation_id = 1. The MAM is built in the global mam, so this is not for
 * use by a daemon that has a cartridge loaded. Returns 0 on success, -1 on
 * error (errno set).
 */
int raw_format(const char *devpath, const char *barcode, const char *density,
	       const char *partspec, uint64_t min_record_size);

#endif /* _CART_BACKEND_RAW_H_ */
