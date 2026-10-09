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

/*
 * Format a block device with a static multi-partition raw cartridge layout.
 *
 *   devpath         block device to format (e.g. /dev/sdX, /dev/loopN)
 *   barcode         cartridge PCL/barcode (copied into the superblock)
 *   partspec        comma-separated per-partition size percentages that must
 *                   total exactly 100, e.g. "92,8" or "85,5,5,5"
 *   min_record_size planning minimum record size; sizes the index region so it
 *                   can describe a data region made entirely of minimum records
 *
 * Writes the initial meta_header for each partition and both superblocks with
 * generation_id = 1. Returns 0 on success, -1 on error (errno set).
 */
int raw_format(const char *devpath, const char *barcode,
	       const char *partspec, uint64_t min_record_size);

#endif /* _CART_BACKEND_RAW_H_ */
