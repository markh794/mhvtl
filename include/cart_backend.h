/*
 * Cartridge storage-backend seam (Phase 2, strangler extract).
 *
 * The emulator's tape semantics (block headers, filemarks, index records,
 * on-media format version) live ABOVE this seam. The backend abstracts only
 * the storage substrate as (stream, partition)-addressed byte channels, so a
 * second adapter (raw block device) can slot in beside the file backend
 * without touching the tape logic.
 *
 * Increment 1 routes the data/index/meta byte I/O of usr/vtlcart.c through
 * this interface. The file backend is behaviour-identical to the previous
 * direct syscalls. Open/close of the underlying files, MAM handling and the
 * v1 legacy format are intentionally left in place for later increments.
 */
#ifndef _CART_BACKEND_H_
#define _CART_BACKEND_H_

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

enum cart_stream {
	CART_STREAM_DATA,	/* payload stream  (datafile[]) */
	CART_STREAM_INDX,	/* block index     (indxfile[]) */
	CART_STREAM_META,	/* filemark/meta   (metafile[]) */
};

struct cart_backend {
	const char *name;
	/* Open/create the storage for one partition. The file backend opens
	 * data.N/indx.N/meta.N; a raw adapter maps the partition to device
	 * regions. (close is handled by a later increment.) */
	int	(*open_partition)(uint8_t partition);
	int	(*create_partition)(int partition);
	ssize_t (*pread)(enum cart_stream s, int partition,
			 void *buf, size_t len, off_t off);
	ssize_t (*pwrite)(enum cart_stream s, int partition,
			  const void *buf, size_t len, off_t off);
	int	(*truncate)(enum cart_stream s, int partition, off_t len);
	int	(*sync)(enum cart_stream s, int partition);
	int	(*fadvise_dontneed)(enum cart_stream s, int partition);
};

/* Active backend (defaults to the file backend). */
extern struct cart_backend *cart_be;

#endif /* _CART_BACKEND_H_ */
