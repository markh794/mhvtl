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

/* open_cartridge() sentinel: the cartridge already exists and is ready, so the
 * caller (create_tape) must NOT re-initialise it. Distinct from the 0/1/2/3
 * success/error codes the cartridge/partition ops otherwise use. */
#define CART_OPEN_EXISTS	100

struct MAM;		/* defined in vtllib.h; only referenced by pointer here */

struct cart_backend {
	const char *name;
	/* Open/create/close the storage for one partition. The file backend
	 * opens data.N/indx.N/meta.N; a raw adapter maps the partition to
	 * device regions. */
	int	(*open_partition)(uint8_t partition);
	int	(*create_partition)(int partition);
	void	(*close_partition)(uint8_t partition);
	ssize_t (*pread)(enum cart_stream s, int partition,
			 void *buf, size_t len, off_t off);
	ssize_t (*pwrite)(enum cart_stream s, int partition,
			  const void *buf, size_t len, off_t off);
	int	(*truncate)(enum cart_stream s, int partition, off_t len);
	int	(*sync)(enum cart_stream s, int partition);
	int	(*fadvise_dontneed)(enum cart_stream s, int partition);

	/*
	 * Cartridge-level lifecycle + MAM (increment 4). Above the per-stream
	 * I/O: the file backend treats a cartridge as a directory holding
	 * mam + mhvtl_data + per-partition data.N/indx.N/meta.N; the raw
	 * backend treats it as one block device whose superblock carries the
	 * partition table and a length-delimited MAM region.
	 */
	/* Open (create==0) or create (create!=0) the cartridge container for
	 * the resolved cartridge. Returns 0 ok, CART_OPEN_EXISTS when a create
	 * finds the cartridge already present (caller returns success without
	 * re-init), or >0 on error (2 create failure, 3 open/corrupt). */
	int	(*open_cartridge)(const char *pcl, int create);
	/* Release the cartridge container (close MAM fds / the device fd). */
	void	(*close_cartridge)(void);
	/* Load the MAM (+ mhvtl_data) into *mamp. Returns 0 ok, 2 format
	 * error (caller honours the bypass-error-check touch file), 3 corrupt. */
	int	(*load_mam)(struct MAM *mamp, uint8_t *sam_stat);
	/* Persist the global MAM. Returns 0 ok, <0 on error. */
	int	(*save_mam)(struct MAM *mamp);
	/* Number of partitions present on the cartridge (<= MAX_PARTITIONS). */
	int	(*num_partitions)(void);
	/* Logical used length (bytes) of a partition's stream: the file backend
	 * fstat()s the stream file; the raw backend returns the partition
	 * table's *_used. Used for EOD/index-length reconstruction on load.
	 * Returns the length, or -1 on error. */
	off_t	(*used_size)(enum cart_stream s, int partition);
	/* Whether a partition's storage is currently open (a cartridge is
	 * loaded and this partition is accessible). The file backend keys on
	 * its per-stream fds; the raw backend on the open device + superblock.
	 * This is the backend-agnostic "medium present" predicate. */
	int	(*partition_loaded)(int partition);
};

/* Active backend (defaults to the file backend). */
extern struct cart_backend *cart_be;

#endif /* _CART_BACKEND_H_ */
